/*-------------------------------------------------------------------------
 *
 * qx_runtime.c
 *	  Stage 7 embedded runtime entry points for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/runtime/qx_runtime.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_database.h"
#include "catalog/pg_qx_agent.h"
#include "catalog/pg_qx_attempt.h"
#include "catalog/pg_qx_checkpoint.h"
#include "catalog/pg_qx_event.h"
#include "catalog/pg_qx_identity.h"
#include "catalog/pg_qx_namespace.h"
#include "catalog/pg_qx_provider.h"
#include "catalog/pg_qx_scheduler_heartbeat.h"
#include "catalog/pg_qx_scheduler_lease.h"
#include "catalog/pg_qx_scheduler_queue.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_qx_step.h"
#include "catalog/pg_qx_task.h"
#include "catalog/pg_qx_trace.h"
#include "common/hmac.h"
#include "common/openssl.h"
#include "common/sha2.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/readfuncs.h"
#include "nodes/value.h"
#include "pgstat.h"
#include "port.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "qx_backend_supervisor.h"
#include "qx_container_backend.h"
#include "qx_microvm_backend.h"
#include "qx_runtime_policy.h"
#include "qx/qx_catalog.h"
#include "qx/qx_stat.h"
#include "qx/qx_recovery.h"
#include "qx/qx_runtime.h"
#include "qx/qx_scheduler.h"
#include "qx/qx_security.h"
#include "qx/qx_semantic_log.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#ifdef USE_OPENSSL
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif

#include <errno.h>
#include <sys/stat.h>

#ifndef WIN32
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <io.h>
#include <windows.h>
#endif

typedef struct QxExternalToolResult
{
	int32		token_charge;
	int32		cost_charge;
	int32		timeout_ms;
	int32		process_limit;
	int32		wall_time_ms;
	bool		path_present;
	bool		restricted_identity;
	char	   *detail;
	char	   *tool_name;
	char	   *principal_name;
	char	   *principal_runtime;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *provider_endpoint;
	char	   *sandbox_name;
	char	   *profile_name;
	char	   *environment_mode;
	char	   *workdir_name;
	char	   *launch_mode;
	char	   *receipt_schema;
	char	   *receipt_alg;
	char	   *receipt_nonce;
	char	   *receipt_signature;
	char	   *attestation_mode;
	char	   *container_id;
	char	   *vm_id;
} QxExternalToolResult;

typedef struct QxSandboxProfile
{
	const char *name;
	int32		timeout_ms;
	int32		memory_kb;
	int32		file_kb;
	int32		max_open_files;
	int32		process_limit;
} QxSandboxProfile;

typedef struct QxSandboxObservation
{
	int32		wall_time_ms;
	const char *launch_mode;
	bool		restricted_identity;
} QxSandboxObservation;

typedef struct QxRuntimeRecoverySnapshot
{
	bool		valid;
	Oid			databaseoid;
	Oid			ownerid;
	QxRecoveryReport report;
} QxRuntimeRecoverySnapshot;

typedef struct QxRuntimeRecoveryQueueMapEntry
{
	Oid			taskoid;
	Oid			queueoid;
} QxRuntimeRecoveryQueueMapEntry;

typedef struct QxRuntimeRecoveryHooksContext
{
	Oid			ownerid;
	Relation	queueledgerrel;
	Relation	leaseledgerrel;
	Relation	heartbeatledgerrel;
	List	   *queue_entries;
} QxRuntimeRecoveryHooksContext;

typedef struct QxRuntimeSchedulerCycleStats
{
	int32		tasks_scanned;
	int32		running_tasks;
	int32		queued_retry_tasks;
	int32		retry_ready_tasks;
	int32		retry_waiting_tasks;
	int32		ownership_skipped;
	int32		leases_seen;
	int32		leases_renewed;
	int32		leases_reclaimed;
	int32		leases_repaired;
	int32		recovery_requeued;
	int32		retry_dispatches;
	int32		dead_lettered_tasks;
	int32		heartbeats_written;
	int32		last_retry_backoff_ms;
	int32		max_retry_backoff_ms;
} QxRuntimeSchedulerCycleStats;

typedef struct QxRuntimeSchedulerDbTarget
{
	Oid			dboid;
	char	   *dbname;
} QxRuntimeSchedulerDbTarget;

typedef struct QxRuntimeSchedulerWorkerKey
{
	Oid			dboid;
	int32		slot_index;
	int32		slot_count;
} QxRuntimeSchedulerWorkerKey;

typedef struct QxRuntimeSchedulerDbWorker
{
	Oid			dboid;
	char	   *dbname;
	int32		slot_index;
	int32		slot_count;
	BackgroundWorkerHandle *handle;
} QxRuntimeSchedulerDbWorker;

static uint32 qx_runtime_temp_seq = 0;
static QxRuntimeRecoverySnapshot qx_runtime_recovery_snapshot = {0};
#define QX_RECEIPT_SIG_HEX_LEN	((PG_SHA256_DIGEST_LENGTH * 2) + 1)
#define QX_ED25519_SIG_BYTES	64
#define QX_ED25519_SIG_HEX_LEN	((QX_ED25519_SIG_BYTES * 2) + 1)
#define QX_MICROVM_TIMEOUT_FLOOR_MS 120000
#define QX_SCHEDULER_LAUNCHER_NAPTIME_MS 3000
#define QX_SCHEDULER_WORKER_NAPTIME_MS 1000
#define QX_SCHEDULER_DB_WORKER_SLOTS_DEFAULT 2
#define QX_SCHEDULER_DB_WORKER_SLOTS_MIN 1
#define QX_SCHEDULER_DB_WORKER_SLOTS_MAX 8
#define QX_SCHEDULER_MAX_RETRIES 3

#ifndef USECS_PER_MSEC
#define USECS_PER_MSEC 1000
#endif

static char *qx_contract_value(const char *contract, const char *key);
static char *qx_provider_receipt_key(Oid provideroid);
static const char *qx_default_runtime_for_provider_kind(const char *provider_kind);
static char *qx_expected_attestation_mode(const char *provider_kind,
										  bool require_attestation);
static char *qx_receipt_hmac_signature_hex(const char *receipt_key,
										   const char *payload);
#ifdef USE_OPENSSL
static char *qx_openssl_error_string(void);
static bool qx_verify_ed25519_receipt_signature(const char *public_key_pem,
												 const char *payload,
												 const char *signature_hex);
#endif
static char *qx_receipt_payload(const char *phase,
								Oid taskoid,
								const char *tool_name,
								const char *principal_name,
								const char *principal_runtime,
								const char *provider_name,
								const char *provider_kind,
								const char *provider_endpoint,
								const char *sandbox_name,
								const char *profile_name,
								const char *environment_mode,
								const char *workdir_name,
								int32 timeout_ms,
								int32 process_limit,
								bool path_present,
								const char *receipt_schema,
								const char *receipt_alg,
								const char *receipt_nonce,
								const char *attestation_mode,
								int32 token_charge,
								int32 cost_charge,
								const char *detail);
static int	qx_sandbox_rank(const char *sandbox_name);
static const char *qx_effective_sandbox_name(const char *tool_sandbox,
											 const char *principal_sandbox);
static const QxSandboxProfile *qx_lookup_sandbox_profile(const char *sandbox_name);
static void qx_runtime_temp_dir(char *path, size_t pathlen);
static void qx_runtime_temp_path(char *path, size_t pathlen,
								 const char *suffix);
static void qx_write_text_file(const char *path, const char *contents);
static void qx_resolve_docker_cli_path(char *resolved, size_t resolved_len);
static const char *qx_default_docker_host(void);
static void qx_resolve_principal_program_path(const char *program_name,
											  char *resolved,
											  size_t resolved_len);
static void qx_resolve_receipt_signer_path(const char *signer_name,
										   char *resolved,
										   size_t resolved_len);
static void qx_launch_principal_program(const char *program_path,
							const QxSandboxProfile *profile,
							const char *runtime_dir,
							const char *request_path,
							const char *response_path,
							const char *launch_request_path,
							bool preserve_host_identity,
							QxSandboxObservation *observation);
static void qx_read_external_result(const char *path,
									QxExternalToolResult *result);
static void qx_build_semantic_execution_metadata(
		QxSemanticExecutionMetadata *metadata,
		const QxExternalToolResult *result);
static void qx_validate_external_result(const QxExternalToolResult *result,
										const QxSandboxProfile *profile,
										const char *phase,
										Oid taskoid,
										const char *expected_tool,
										const char *expected_principal,
										const char *expected_principal_runtime,
										const char *expected_provider,
										const char *expected_provider_kind,
										const char *expected_provider_endpoint,
										const char *expected_receipt_schema,
										const char *expected_receipt_alg,
										const char *expected_receipt_nonce,
										const char *receipt_key,
										bool require_attestation);
static void qx_free_external_result(QxExternalToolResult *result);
static void qx_execute_tool_contract(const char *contract,
									 const char *phase,
									 Oid taskoid,
									 const char *goal,
									 bool input_present,
									 QxExternalToolResult *result);
static List *qx_parse_task_authorized_contracts(const char *serialized);
static const char *qx_runtime_request_phase_contract(
	const QxRuntimeTaskRequest *request,
	bool prefer_resume);
static const char *qx_safe_runtime_text(const char *value);
static const char *qx_bool_literal(bool value);
static const char *qx_effective_priority_name(const char *priority);
static char *qx_fetch_agent_name(Oid agentoid);
static void qx_scheduler_apply_contract(QxSchedulerTaskEnvelope *envelope,
										const char *contract);
static void qx_scheduler_fill_envelope(QxSchedulerTaskEnvelope *envelope,
									   Oid ownerid,
									   Oid sessionoid,
									   Oid agentoid,
									   Oid identityoid,
									   Oid namespace_policy_oid,
									   Oid taskoid,
									   Oid attemptoid,
									   const char *task_name,
									   const char *agent_name,
									   const char *identity_name,
									   const char *namespace_policy_name,
									   const char *priority,
									   const char *checkpoint_label,
									   const char *resume_label,
									   bool resumable,
									   bool checkpointable,
									   bool is_retry,
									   int32 budget_tokens,
									   int32 budget_cost,
									   int32 estimated_tokens,
									   int32 estimated_cost,
									   int32 retry_count,
									   const char *contract);
static char *qx_scheduler_append_runtime_payload(
		char *base_payload,
		const QxSchedulerTaskEnvelope *envelope,
		bool include_lease);
extern bool QxSchedulerLeaseNeedsReclaim(const QxSchedulerLeaseSnapshot *lease,
										 TimestampTz now);
static char *qx_runtime_recovery_selected_contract(const QxCatalogTaskInfo *task,
												   bool prefer_resume);
static char *qx_runtime_task_phase_contract(const QxCatalogTaskInfo *task,
											bool prefer_resume);
static char *qx_runtime_recovery_checkpoint_label(Oid checkpointoid);
static void qx_runtime_fill_recovery_scheduler_envelope(
	QxSchedulerTaskEnvelope *envelope,
	const QxCatalogTaskInfo *task,
	Oid attemptoid,
	int32 retry_count,
	const char *checkpoint_label,
	const char *resume_label,
	bool resumable,
	bool checkpointable,
	bool is_retry,
	const char *contract);
static void qx_runtime_remember_recovery_queueoid(
	QxRuntimeRecoveryHooksContext *context,
	Oid taskoid,
	Oid queueoid);
static Oid qx_runtime_lookup_recovery_queueoid(
	const QxRuntimeRecoveryHooksContext *context,
	Oid taskoid);
static void qx_runtime_fill_task_insert_params(QxCatalogTaskInsertParams *params,
											   const QxRuntimeTaskRequest *request);

static int32 qx_runtime_lease_ttl_ms(
	const QxSchedulerLeaseSnapshot *snapshot);
static int32 qx_runtime_scheduler_max_retries(void);
static int32 qx_runtime_scheduler_slot_count(void);
static int32 qx_runtime_scheduler_owner_slot(Oid taskoid, int32 slot_count);
static bool qx_runtime_scheduler_worker_owns_task(Oid taskoid,
												  int32 slot_index,
												  int32 slot_count);
static int32 qx_runtime_retry_backoff_ms(const char *priority,
										 int32 retry_count,
										 int32 lease_ttl_ms,
										 Oid taskoid);
static void qx_runtime_set_heartbeat_receipt(
	QxSchedulerHeartbeatSnapshot *heartbeat,
	const char *receipt_mode);
static void qx_runtime_free_scheduler_queue_snapshot(
	QxSchedulerQueueSnapshot *snapshot);
static void qx_runtime_free_scheduler_lease_snapshot(
	QxSchedulerLeaseSnapshot *snapshot);

static List *qx_runtime_scheduler_db_targets(void);
static bool qx_runtime_scheduler_db_target_allowed(const char *dbname);
static void qx_runtime_free_scheduler_db_targets(List *targets);
static QxRuntimeSchedulerDbWorker *qx_runtime_find_scheduler_db_worker(
	const List *workers,
	Oid dboid,
	int32 slot_index);
static bool qx_runtime_launch_scheduler_db_worker(
	Oid dboid,
	const char *dbname,
	int32 slot_index,
	int32 slot_count,
	BackgroundWorkerHandle **handle);
static void qx_runtime_scheduler_heartbeat_wait(uint32 *wait_event,
												const char *wait_name,
												long timeout_ms);
static Oid qx_runtime_test_start_uncheckpointed_task(Oid sessionoid,
													 const char *priority);
static Oid qx_runtime_test_start_exhausted_retry_task(Oid sessionoid);
static void qx_runtime_run_scheduler_cycle(QxRuntimeSchedulerCycleStats *stats,
										   int32 slot_index,
										   int32 slot_count);
static void qx_runtime_run_scheduler_retry_cycle(QxRuntimeSchedulerCycleStats *stats,
												 int32 slot_index,
												 int32 slot_count);
static bool qx_runtime_reclaim_running_attempt(
	Relation taskrel,
	Relation attemptrel,
	Relation queueledgerrel,
	Relation leaseledgerrel,
	Relation heartbeatledgerrel,
	const QxCatalogTaskInfo *task,
	const QxCatalogAttemptInfo *attempt,
	const QxSchedulerLeaseSnapshot *lease_snapshot,
	Oid queueoid,
	const char *worker_name,
	QxRuntimeSchedulerCycleStats *stats);
static bool qx_runtime_dispatch_retry_task(
	Relation taskrel,
	Relation attemptrel,
	Relation steprel,
	Relation eventrel,
	Relation tracerel,
	Relation checkpointrel,
	Relation queueledgerrel,
	Relation leaseledgerrel,
	Relation heartbeatledgerrel,
	const QxCatalogTaskInfo *task,
	const QxCatalogAttemptInfo *failed_attempt,
	const QxSchedulerQueueSnapshot *retry_queue,
	const char *worker_name,
	QxRuntimeSchedulerCycleStats *stats);
static bool qx_runtime_dead_letter_retry_task(
	Relation taskrel,
	Relation attemptrel,
	Relation eventrel,
	Relation tracerel,
	Relation queueledgerrel,
	const QxCatalogTaskInfo *task,
	const QxCatalogAttemptInfo *failed_attempt,
	const QxSchedulerQueueSnapshot *retry_queue,
	const char *worker_name,
	QxRuntimeSchedulerCycleStats *stats);
static bool qx_runtime_retry_candidate_better(
	const QxCatalogTaskInfo *candidate_task,
	const QxCatalogAttemptInfo *candidate_attempt,
	const QxSchedulerQueueSnapshot *candidate_queue,
	const QxCatalogTaskInfo *best_task,
	const QxCatalogAttemptInfo *best_attempt,
	const QxSchedulerQueueSnapshot *best_queue);
static void qx_runtime_log_scheduler_cycle(const char *worker_name,
										   const QxRuntimeSchedulerCycleStats *stats);
static void qx_runtime_recovery_begin(const QxRecoveryReport *report,
									  void *userdata);
static void qx_runtime_recovery_requeue_task(
	const QxRecoveryTaskSummary *summary,
	void *userdata);
static void qx_runtime_recovery_fence_attempt(
	const QxRecoveryAttemptSummary *summary,
	void *userdata);
static void qx_runtime_recovery_finish(const QxRecoveryReport *report,
									   void *userdata);
static void qx_runtime_ensure_startup_recovery_scan(Oid ownerid);
static char *qx_runtime_failover_recovery_payload(
	char *base_payload,
	const QxRecoveryReport *report);
static char *qx_runtime_append_recovery_payload(char *base_payload);
static bool qx_runtime_path_exists(const char *path);
static bool qx_resolve_microvm_runtime_paths(char *kernel_path,
											 size_t kernel_path_len,
											 char *initrd_path,
											 size_t initrd_path_len);
static void qx_resolve_microvm_qemu_path(char *resolved, size_t resolved_len);
static void qx_validate_container_runtime_contract(
	const char *phase,
	Oid taskoid,
	const char *tool_name,
	const char *handler_name,
	const char *effective_sandbox,
	const QxSandboxProfile *profile,
	const char *principal_name,
	const char *principal_runtime,
	const char *provider_name,
	const char *provider_kind,
	const char *provider_endpoint,
	const char *receipt_schema,
	const char *receipt_alg,
	const char *receipt_nonce,
	bool require_attestation);
static void qx_validate_microvm_runtime_contract(
	const char *phase,
	Oid taskoid,
	const char *tool_name,
	const char *handler_name,
	const char *effective_sandbox,
	const QxSandboxProfile *profile,
	const char *principal_name,
	const char *principal_runtime,
	const char *provider_name,
	const char *provider_kind,
	const char *provider_endpoint,
	const char *receipt_schema,
	const char *receipt_alg,
	const char *receipt_nonce,
	bool require_attestation);


static const char *
qx_safe_runtime_text(const char *value)
{
	if (value == NULL || value[0] == '\0')
		return "<unknown>";

	return value;
}

static const char *
qx_bool_literal(bool value)
{
	return value ? "true" : "false";
}

static const char *
qx_effective_priority_name(const char *priority)
{
	if (priority == NULL || priority[0] == '\0')
		return "normal";

	return priority;
}

static char *
qx_fetch_agent_name(Oid agentoid)
{
	QxCatalogAgentInfo agent;
	char	   *name;

	if (!QxCatalogLookupAgentByOid(agentoid, &agent))
		elog(ERROR, "cache lookup failed for QhapaqXian agent %u", agentoid);

	name = pstrdup(agent.name);
	QxCatalogFreeAgentInfo(&agent);

	return name;
}

static char *
qx_contract_value(const char *contract, const char *key)
{
	char	   *pattern;
	char	   *start;
	char	   *end;
	char	   *value;

	if (contract == NULL || key == NULL)
		return NULL;

	pattern = psprintf("%s=", key);
	start = strstr(contract, pattern);
	pfree(pattern);
	if (start == NULL)
		return NULL;

	start += strlen(key) + 1;
	end = strchr(start, ';');
	if (end == NULL)
		end = unconstify(char *, contract) + strlen(contract);

	value = pnstrdup(start, end - start);
	return value;
}

static void
qx_scheduler_apply_contract(QxSchedulerTaskEnvelope *envelope,
							const char *contract)
{
	if (envelope == NULL || contract == NULL)
		return;

	envelope->principal_name = qx_contract_value(contract, "principal");
	envelope->provider_name = qx_contract_value(contract, "provider");
	envelope->provider_kind = qx_contract_value(contract, "provider_kind");
	envelope->principal_runtime = qx_contract_value(contract,
													 "principal_runtime");

	if (envelope->principal_runtime == NULL)
		envelope->principal_runtime = pstrdup(
			qx_default_runtime_for_provider_kind(envelope->provider_kind));
}

static void
qx_scheduler_fill_envelope(QxSchedulerTaskEnvelope *envelope,
						   Oid ownerid,
						   Oid sessionoid,
						   Oid agentoid,
						   Oid identityoid,
						   Oid namespace_policy_oid,
						   Oid taskoid,
						   Oid attemptoid,
						   const char *task_name,
						   const char *agent_name,
						   const char *identity_name,
						   const char *namespace_policy_name,
						   const char *priority,
						   const char *checkpoint_label,
						   const char *resume_label,
						   bool resumable,
						   bool checkpointable,
						   bool is_retry,
						   int32 budget_tokens,
						   int32 budget_cost,
						   int32 estimated_tokens,
						   int32 estimated_cost,
						   int32 retry_count,
						   const char *contract)
{
	const char *effective_priority;

	Assert(envelope != NULL);

	QxSchedulerInitTaskEnvelope(envelope);
	effective_priority = qx_effective_priority_name(priority);

	envelope->ownerid = ownerid;
	envelope->sessionoid = sessionoid;
	envelope->agentoid = agentoid;
	envelope->identityoid = identityoid;
	envelope->namespace_policy_oid = namespace_policy_oid;
	envelope->taskoid = taskoid;
	envelope->attemptoid = attemptoid;
	envelope->task_name = pstrdup(qx_safe_runtime_text(task_name));
	envelope->agent_name = pstrdup(qx_safe_runtime_text(agent_name));
	envelope->identity_name = pstrdup(qx_safe_runtime_text(identity_name));
	envelope->namespace_policy_name =
		pstrdup(qx_safe_runtime_text(namespace_policy_name));
	envelope->priority = pstrdup(effective_priority);
	envelope->checkpoint_label = checkpoint_label != NULL ?
		pstrdup(checkpoint_label) : NULL;
	envelope->resume_label = resume_label != NULL ?
		pstrdup(resume_label) : NULL;
	envelope->resumable = resumable;
	envelope->checkpointable = checkpointable;
	envelope->urgent = (QxSchedulerPriorityWeight(effective_priority) >= 80);
	envelope->requires_heartbeat = true;
	envelope->is_retry = is_retry;
	envelope->budget_tokens = budget_tokens;
	envelope->budget_cost = budget_cost;
	envelope->estimated_tokens = estimated_tokens;
	envelope->estimated_cost = estimated_cost;
	envelope->retry_count = retry_count;
	envelope->max_retries = qx_runtime_scheduler_max_retries();
	envelope->queue_name = QxSchedulerBuildQueueKey(
		envelope->namespace_policy_name,
		envelope->priority,
		envelope->agent_name);

	qx_scheduler_apply_contract(envelope, contract);

	envelope->heartbeat_interval_ms =
		QxSchedulerDefaultHeartbeatIntervalMs(envelope);
	envelope->lease_ttl_ms = QxSchedulerDefaultLeaseTtlMs(envelope);
}

static char *
qx_scheduler_append_runtime_payload(char *base_payload,
									const QxSchedulerTaskEnvelope *envelope,
									bool include_lease)
{
	QxSchedulerQueueSnapshot *queue_snapshot;
	QxSchedulerLeaseSnapshot *lease_snapshot = NULL;
	QxSchedulerHeartbeatSnapshot *heartbeat_snapshot = NULL;
	StringInfoData buf;
	TimestampTz	now;

	Assert(envelope != NULL);

	if (!QxSchedulerRuntimeContractIsValid(envelope))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("scheduler contract is incomplete for runtime admission"),
				 errdetail("Stage 26 admission now requires a valid scheduler envelope before execution proceeds.")));

	queue_snapshot = QxSchedulerQueueSnapshotFromEnvelope(envelope);
	if (include_lease)
	{
		lease_snapshot = QxSchedulerLeaseSnapshotFromEnvelope(envelope,
															 InvalidOid,
															 "embedded-runtime");
		heartbeat_snapshot = QxSchedulerHeartbeatSnapshotFromLease(lease_snapshot);
	}

	initStringInfo(&buf);
	appendStringInfoString(&buf, base_payload != NULL ? base_payload : "");
	appendStringInfo(&buf,
					 ";scheduler_queue=%s;scheduler_queue_kind=%d;scheduler_priority_weight=%d;scheduler_runtime=%s;scheduler_provider=%s;scheduler_provider_kind=%s;scheduler_resumable=%s;scheduler_checkpointable=%s;scheduler_urgent=%s;scheduler_retry=%d/%d;scheduler_heartbeat_ms=%d;scheduler_lease_ttl_ms=%d",
					 qx_safe_runtime_text(queue_snapshot->queue_name),
					 (int) queue_snapshot->queue_kind,
					 QxSchedulerPriorityWeight(envelope->priority),
					 qx_safe_runtime_text(envelope->principal_runtime),
					 qx_safe_runtime_text(envelope->provider_name),
					 qx_safe_runtime_text(envelope->provider_kind),
					 qx_bool_literal(envelope->resumable),
					 qx_bool_literal(envelope->checkpointable),
					 qx_bool_literal(envelope->urgent),
					 envelope->retry_count,
					 envelope->max_retries,
					 envelope->heartbeat_interval_ms,
					 envelope->lease_ttl_ms);

	if (envelope->checkpoint_label != NULL)
		appendStringInfo(&buf, ";scheduler_checkpoint=%s",
						 qx_safe_runtime_text(envelope->checkpoint_label));
	if (envelope->resume_label != NULL)
		appendStringInfo(&buf, ";scheduler_resume=%s",
						 qx_safe_runtime_text(envelope->resume_label));

	if (include_lease)
	{
		now = GetCurrentTimestamp();
		appendStringInfo(&buf,
						 ";scheduler_worker=%s;scheduler_lease_state=%d;scheduler_lease_expired=%s;scheduler_heartbeat_state=%d;scheduler_heartbeat_stale=%s;scheduler_receipt_mode=%s",
						 qx_safe_runtime_text(lease_snapshot->worker_name),
						 (int) lease_snapshot->state,
						 qx_bool_literal(QxSchedulerLeaseExpired(lease_snapshot, now)),
						 (int) heartbeat_snapshot->state,
						 qx_bool_literal(QxSchedulerHeartbeatStale(heartbeat_snapshot,
																  now)),
						 qx_safe_runtime_text(heartbeat_snapshot->receipt_mode));
	}

	if (base_payload != NULL)
		pfree(base_payload);

	return buf.data;
}

static char *
qx_runtime_recovery_selected_contract(const QxCatalogTaskInfo *task,
									  bool prefer_resume)
{
	return qx_runtime_task_phase_contract(task, prefer_resume);
}

static char *
qx_runtime_recovery_checkpoint_label(Oid checkpointoid)
{
	QxCatalogCheckpointInfo checkpoint;
	char	   *label = NULL;

	if (!OidIsValid(checkpointoid))
		return NULL;

	if (!QxCatalogLookupCheckpointByOid(checkpointoid, &checkpoint))
		return NULL;

	if (checkpoint.label != NULL)
		label = pstrdup(checkpoint.label);

	QxCatalogFreeCheckpointInfo(&checkpoint);

	return label;
}

static void
qx_runtime_fill_recovery_scheduler_envelope(
	QxSchedulerTaskEnvelope *envelope,
	const QxCatalogTaskInfo *task,
	Oid attemptoid,
	int32 retry_count,
	const char *checkpoint_label,
	const char *resume_label,
	bool resumable,
	bool checkpointable,
	bool is_retry,
	const char *contract)
{
	Assert(envelope != NULL);
	Assert(task != NULL);

	qx_scheduler_fill_envelope(envelope,
							   task->ownerid,
							   task->sessionoid,
							   task->agentoid,
							   task->identityoid,
							   task->namespacepolicyoid,
							   task->oid,
							   attemptoid,
							   task->name,
							   task->agent_name,
							   task->identity_name,
							   task->policy_name,
							   task->priority,
							   checkpoint_label,
							   resume_label,
							   resumable,
							   checkpointable,
							   is_retry,
							   task->budget_tokens,
							   task->budget_cost,
							   task->estimated_tokens,
							   task->estimated_cost,
							   retry_count,
							   contract);
}

static void
qx_runtime_remember_recovery_queueoid(QxRuntimeRecoveryHooksContext *context,
									  Oid taskoid,
									  Oid queueoid)
{
	ListCell   *lc;

	if (context == NULL || !OidIsValid(taskoid) || !OidIsValid(queueoid))
		return;

	foreach(lc, context->queue_entries)
	{
		QxRuntimeRecoveryQueueMapEntry *entry = lfirst(lc);

		if (entry->taskoid == taskoid)
		{
			entry->queueoid = queueoid;
			return;
		}
	}

	{
		QxRuntimeRecoveryQueueMapEntry *entry;

		entry = palloc0(sizeof(QxRuntimeRecoveryQueueMapEntry));
		entry->taskoid = taskoid;
		entry->queueoid = queueoid;
		context->queue_entries = lappend(context->queue_entries, entry);
	}
}

static Oid
qx_runtime_lookup_recovery_queueoid(const QxRuntimeRecoveryHooksContext *context,
									Oid taskoid)
{
	ListCell   *lc;

	if (context == NULL || !OidIsValid(taskoid))
		return InvalidOid;

	foreach(lc, context->queue_entries)
	{
		QxRuntimeRecoveryQueueMapEntry *entry = lfirst(lc);

		if (entry->taskoid == taskoid)
			return entry->queueoid;
	}

	return InvalidOid;
}

static void
qx_runtime_recovery_begin(const QxRecoveryReport *report, void *userdata)
{
	QxRuntimeRecoveryHooksContext *context = userdata;

	(void) report;

	if (context == NULL)
		return;

	context->queue_entries = NIL;
	context->queueledgerrel = table_open(QxSchedulerQueueRelationId,
										 RowExclusiveLock);
	context->leaseledgerrel = table_open(QxSchedulerLeaseRelationId,
										 RowExclusiveLock);
	context->heartbeatledgerrel = table_open(QxSchedulerHeartbeatRelationId,
											 RowExclusiveLock);
}

static void
qx_runtime_recovery_requeue_task(const QxRecoveryTaskSummary *summary,
								 void *userdata)
{
	QxRuntimeRecoveryHooksContext *context = userdata;
	QxCatalogTaskInfo task;
	QxSchedulerTaskEnvelope scheduler_envelope;
	QxSchedulerQueueSnapshot *queue_snapshot;
	char	   *selected_contract;
	char	   *checkpoint_label;
	char	   *resume_label;
	Oid			queueoid;
	Oid			attemptoid;
	bool		prefer_resume;
	int32		retry_count;

	if (context == NULL || summary == NULL || !OidIsValid(summary->taskoid))
		return;

	if (!OidIsValid(summary->lastattemptoid))
		return;

	if (!QxCatalogLookupTaskByOid(summary->taskoid, &task))
		elog(ERROR, "cache lookup failed for QhapaqXian recovery task %u",
			 summary->taskoid);

	prefer_resume = OidIsValid(summary->lastcheckpointoid);
	checkpoint_label =
		qx_runtime_recovery_checkpoint_label(summary->lastcheckpointoid);
	resume_label = checkpoint_label != NULL ? pstrdup(checkpoint_label) : NULL;
	selected_contract =
		qx_runtime_recovery_selected_contract(&task, prefer_resume);
	attemptoid = summary->lastattemptoid;
	retry_count = Max(summary->attempt_count, 0);

	qx_runtime_fill_recovery_scheduler_envelope(&scheduler_envelope,
												&task,
												attemptoid,
												retry_count,
												checkpoint_label,
												resume_label,
												true,
												true,
												(retry_count > 0),
												selected_contract);
	queue_snapshot = QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
	queue_snapshot->queue_kind = QX_SCHEDULER_QUEUE_RECOVERY;
	queue_snapshot->retry_count = retry_count;
	queueoid = QxCatalogInsertSchedulerQueue(context->queueledgerrel, queue_snapshot);
	CommandCounterIncrement();
	qx_runtime_remember_recovery_queueoid(context, summary->taskoid, queueoid);

	if (selected_contract != NULL)
		pfree(selected_contract);
	if (checkpoint_label != NULL)
		pfree(checkpoint_label);
	if (resume_label != NULL)
		pfree(resume_label);
	QxCatalogFreeTaskInfo(&task);
}

static void
qx_runtime_recovery_fence_attempt(const QxRecoveryAttemptSummary *summary,
								  void *userdata)
{
	QxRuntimeRecoveryHooksContext *context = userdata;
	QxCatalogTaskInfo task;
	QxSchedulerTaskEnvelope scheduler_envelope;
	QxSchedulerQueueSnapshot *queue_snapshot;
	QxSchedulerLeaseSnapshot *lease_snapshot;
	QxSchedulerLeaseSnapshot *reclaimed_lease_snapshot;
	QxSchedulerHeartbeatSnapshot *heartbeat_snapshot;
	char	   *selected_contract;
	char	   *checkpoint_label = NULL;
	char	   *resume_label = NULL;
	Oid			queueoid;
	Oid			leaseoid;
	bool		prefer_resume;

	if (context == NULL || summary == NULL || !OidIsValid(summary->attemptoid) ||
		!OidIsValid(summary->taskoid))
		return;

	if (!QxCatalogLookupTaskByOid(summary->taskoid, &task))
		elog(ERROR, "cache lookup failed for QhapaqXian recovery task %u",
			 summary->taskoid);

	prefer_resume = (summary->strategy != NULL &&
					 strcmp(summary->strategy, "resume") == 0);
	if (OidIsValid(task.lastcheckpointid))
	{
		checkpoint_label =
			qx_runtime_recovery_checkpoint_label(task.lastcheckpointid);
		if (prefer_resume && checkpoint_label != NULL)
			resume_label = pstrdup(checkpoint_label);
	}
	selected_contract =
		qx_runtime_recovery_selected_contract(&task, prefer_resume);
	qx_runtime_fill_recovery_scheduler_envelope(&scheduler_envelope,
												&task,
												summary->attemptoid,
												Max(summary->seqno, 0),
												checkpoint_label,
												resume_label,
												prefer_resume,
												true,
												(summary->seqno > 1),
												selected_contract);

	queueoid = qx_runtime_lookup_recovery_queueoid(context, summary->taskoid);
	if (!OidIsValid(queueoid))
	{
		queue_snapshot = QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
		queue_snapshot->queue_kind = QX_SCHEDULER_QUEUE_RECOVERY;
		queue_snapshot->retry_count = Max(summary->seqno, 0);
		queueoid = QxCatalogInsertSchedulerQueue(context->queueledgerrel,
											 queue_snapshot);
		CommandCounterIncrement();
		qx_runtime_remember_recovery_queueoid(context, summary->taskoid, queueoid);
	}

	lease_snapshot = QxSchedulerLeaseSnapshotFromEnvelope(&scheduler_envelope,
														  InvalidOid,
														  "embedded-runtime");
	reclaimed_lease_snapshot =
		QxSchedulerReleaseLeaseSnapshot(lease_snapshot, true);
	leaseoid = QxCatalogInsertSchedulerLease(context->leaseledgerrel,
										 queueoid,
										 reclaimed_lease_snapshot);
	heartbeat_snapshot =
		QxSchedulerFinalizeHeartbeatSnapshot(reclaimed_lease_snapshot,
											 "scheduler-reclaim");
	heartbeat_snapshot->state = QX_SCHEDULER_HEARTBEAT_MISSED;
	heartbeat_snapshot->lag_ms = scheduler_envelope.heartbeat_interval_ms;
	heartbeat_snapshot->stale = true;
	(void) QxCatalogInsertSchedulerHeartbeat(context->heartbeatledgerrel,
										 leaseoid,
										 heartbeat_snapshot);
	CommandCounterIncrement();

	if (selected_contract != NULL)
		pfree(selected_contract);
	if (checkpoint_label != NULL)
		pfree(checkpoint_label);
	if (resume_label != NULL)
		pfree(resume_label);
	QxCatalogFreeTaskInfo(&task);
}

static void
qx_runtime_recovery_finish(const QxRecoveryReport *report, void *userdata)
{
	QxRuntimeRecoveryHooksContext *context = userdata;

	if (report != NULL)
		QxStatReportRecoveryScan(MyDatabaseId, report, report->failover_rebuild);

	if (context == NULL)
		return;

	if (context->queue_entries != NIL)
		list_free_deep(context->queue_entries);
	context->queue_entries = NIL;

	if (context->heartbeatledgerrel != NULL)
		table_close(context->heartbeatledgerrel, RowExclusiveLock);
	if (context->leaseledgerrel != NULL)
		table_close(context->leaseledgerrel, RowExclusiveLock);
	if (context->queueledgerrel != NULL)
		table_close(context->queueledgerrel, RowExclusiveLock);

	context->heartbeatledgerrel = NULL;
	context->leaseledgerrel = NULL;
	context->queueledgerrel = NULL;
}

static void
qx_runtime_ensure_startup_recovery_scan(Oid ownerid)
{
	QxRecoveryStartupRequest request;
	QxRecoveryHooks hooks;
	QxRecoveryReport *report;
	QxRuntimeRecoveryHooksContext hook_context;
	Oid			effective_ownerid;

	effective_ownerid = OidIsValid(ownerid) ? ownerid : GetUserId();
	if (qx_runtime_recovery_snapshot.valid &&
		qx_runtime_recovery_snapshot.databaseoid == MyDatabaseId &&
		qx_runtime_recovery_snapshot.ownerid == effective_ownerid)
		return;

	memset(&request, 0, sizeof(request));
	request.databaseoid = MyDatabaseId;
	request.ownerid = effective_ownerid;
	request.include_attempts = true;
	request.include_checkpoints = true;
	request.fence_stale_attempts = true;
	request.requeue_checkpointed_tasks = true;
	request.rebuild_from_semantic_log = false;

	memset(&hooks, 0, sizeof(hooks));
	memset(&hook_context, 0, sizeof(hook_context));
	hook_context.ownerid = effective_ownerid;
	hooks.begin = qx_runtime_recovery_begin;
	hooks.requeue_task = qx_runtime_recovery_requeue_task;
	hooks.fence_attempt = qx_runtime_recovery_fence_attempt;
	hooks.finish = qx_runtime_recovery_finish;
	hooks.userdata = &hook_context;

	report = QxRecoveryRunStartupScan(&request, &hooks);
	qx_runtime_recovery_snapshot.valid = true;
	qx_runtime_recovery_snapshot.databaseoid = MyDatabaseId;
	qx_runtime_recovery_snapshot.ownerid = effective_ownerid;
	if (report != NULL)
	{
		qx_runtime_recovery_snapshot.report = *report;
		QxRecoveryFreeReport(report);
	}
	else
		memset(&qx_runtime_recovery_snapshot.report, 0,
			   sizeof(qx_runtime_recovery_snapshot.report));
}

static char *
qx_runtime_failover_recovery_payload(char *base_payload,
									 const QxRecoveryReport *report)
{
	StringInfoData buf;

	if (report == NULL)
		elog(ERROR, "failover recovery report cannot be null");

	initStringInfo(&buf);
	appendStringInfoString(&buf, base_payload != NULL ? base_payload : "");
	appendStringInfo(&buf,
					 ";recovery_startup_scan=%s;recovery_failover_rebuild=%s;recovery_tasks_scanned=%d;recovery_attempts_scanned=%d;recovery_checkpoints_scanned=%d;recovery_tasks_requeued=%d;recovery_attempts_fenced=%d;recovery_checkpoints_replayed=%d;recovery_orphan_attempts=%d;recovery_semantic_candidates=%d;recovery_tasks_requeue_suppressed=%d;recovery_attempts_fence_suppressed=%d",
					 qx_bool_literal(report->startup_scan),
					 qx_bool_literal(report->failover_rebuild),
					 report->tasks_scanned,
					 report->attempts_scanned,
					 report->checkpoints_scanned,
					 report->tasks_requeued,
					 report->attempts_fenced,
					 report->checkpoints_replayed,
					 report->orphan_attempts,
					 report->semantic_replay_candidates,
					 report->tasks_requeue_suppressed,
					 report->attempts_fence_suppressed);

	if (base_payload != NULL)
		pfree(base_payload);

	return buf.data;
}

static char *
qx_runtime_append_recovery_payload(char *base_payload)
{
	StringInfoData buf;
	const QxRecoveryReport *report;

	report = &qx_runtime_recovery_snapshot.report;
	initStringInfo(&buf);
	appendStringInfoString(&buf, base_payload != NULL ? base_payload : "");
	appendStringInfo(&buf,
					 ";recovery_startup_scan=%s;recovery_tasks_scanned=%d;recovery_attempts_scanned=%d;recovery_checkpoints_scanned=%d;recovery_tasks_requeued=%d;recovery_attempts_fenced=%d;recovery_orphan_attempts=%d;recovery_semantic_candidates=%d;recovery_tasks_requeue_suppressed=%d;recovery_attempts_fence_suppressed=%d",
					 qx_bool_literal(report->startup_scan),
					 report->tasks_scanned,
					 report->attempts_scanned,
					 report->checkpoints_scanned,
					 report->tasks_requeued,
					 report->attempts_fenced,
					 report->orphan_attempts,
					 report->semantic_replay_candidates,
					 report->tasks_requeue_suppressed,
					 report->attempts_fence_suppressed);

	if (base_payload != NULL)
		pfree(base_payload);

	return buf.data;
}

static void
qx_validate_container_runtime_contract(const char *phase,
										 Oid taskoid,
										 const char *tool_name,
										 const char *handler_name,
										 const char *effective_sandbox,
										 const QxSandboxProfile *profile,
										 const char *principal_name,
										 const char *principal_runtime,
										 const char *provider_name,
										 const char *provider_kind,
										 const char *provider_endpoint,
										 const char *receipt_schema,
										 const char *receipt_alg,
										 const char *receipt_nonce,
										 bool require_attestation)
{
	QxContainerBackendRequest request;
	char	   *image_ref;

	if (!qx_container_backend_is_supported_provider_kind(provider_kind) &&
		!qx_container_backend_is_supported_runtime_class(principal_runtime))
		return;

	qx_container_backend_request_init(&request);
	image_ref = pstrdup(getenv("QX_CONTAINER_IMAGE") != NULL ?
						getenv("QX_CONTAINER_IMAGE") : "alpine:3.20");

	request.phase = pstrdup(phase != NULL ? phase : "submit");
	request.tool_name = pstrdup(qx_safe_runtime_text(tool_name));
	request.principal_name = pstrdup(qx_safe_runtime_text(principal_name));
	request.principal_runtime = pstrdup(qx_safe_runtime_text(principal_runtime));
	request.provider_name = pstrdup(qx_safe_runtime_text(provider_name));
	request.provider_kind = pstrdup(qx_safe_runtime_text(provider_kind));
	request.provider_endpoint = pstrdup(qx_safe_runtime_text(provider_endpoint));
	request.sandbox_name = pstrdup(qx_safe_runtime_text(effective_sandbox));
	request.profile_name = pstrdup(profile->name);
	request.environment_mode = pstrdup("minimal");
	request.workdir_name = pstrdup("pg_qx_runtime");
	request.command_line = psprintf("docker run --rm %s true", image_ref);
	request.image_ref = image_ref;
	request.receipt_schema = pstrdup(qx_safe_runtime_text(receipt_schema));
	request.receipt_alg = pstrdup(qx_safe_runtime_text(receipt_alg));
	request.receipt_nonce = pstrdup(qx_safe_runtime_text(receipt_nonce));
	request.attestation_mode =
		pstrdup(qx_container_backend_expected_attestation_mode());
	request.detail = psprintf("task=%u;handler=%s",
							  taskoid,
							  handler_name != NULL ? handler_name : "<unknown>");
	request.timeout_ms = profile->timeout_ms;
	request.memory_kb = profile->memory_kb;
	request.process_limit = profile->process_limit;
	request.token_charge = 0;
	request.cost_charge = 0;
	request.require_attestation = require_attestation;
	request.allow_network = false;
	request.allow_privilege_escalation = false;

	qx_container_backend_validate_request(&request);
	qx_container_backend_request_free(&request);
}

static void
qx_validate_microvm_runtime_contract(const char *phase,
									 Oid taskoid,
									 const char *tool_name,
									 const char *handler_name,
									 const char *effective_sandbox,
									 const QxSandboxProfile *profile,
									 const char *principal_name,
									 const char *principal_runtime,
									 const char *provider_name,
									 const char *provider_kind,
									 const char *provider_endpoint,
									 const char *receipt_schema,
									 const char *receipt_alg,
									 const char *receipt_nonce,
									 bool require_attestation)
{
	QxMicrovmBackendRequest request;
	char		kernel_path[MAXPGPATH];
	char		initrd_path[MAXPGPATH];
	char		qemu_path[MAXPGPATH];
	char	   *image_ref;
	char	   *accel;

	if (!qx_microvm_backend_is_supported_provider_kind(provider_kind) &&
		!qx_microvm_backend_is_supported_runtime_class(principal_runtime))
		return;

	if (!qx_resolve_microvm_runtime_paths(kernel_path, sizeof(kernel_path),
										  initrd_path, sizeof(initrd_path)))
		return;

	qx_microvm_backend_request_init(&request);
	image_ref = psprintf("kernel=%s;initrd=%s", kernel_path, initrd_path);
	accel = pstrdup(getenv("QX_MICROVM_ACCEL") != NULL ?
					getenv("QX_MICROVM_ACCEL") : "tcg");
	qx_resolve_microvm_qemu_path(qemu_path, sizeof(qemu_path));

	request.phase = pstrdup(phase != NULL ? phase : "submit");
	request.tool_name = pstrdup(qx_safe_runtime_text(tool_name));
	request.principal_name = pstrdup(qx_safe_runtime_text(principal_name));
	request.principal_runtime = pstrdup(qx_safe_runtime_text(principal_runtime));
	request.provider_name = pstrdup(qx_safe_runtime_text(provider_name));
	request.provider_kind = pstrdup(qx_safe_runtime_text(provider_kind));
	request.provider_endpoint = pstrdup(qx_safe_runtime_text(provider_endpoint));
	request.sandbox_name = pstrdup(qx_safe_runtime_text(effective_sandbox));
	request.profile_name = pstrdup(profile->name);
	request.environment_mode = pstrdup("minimal");
	request.workdir_name = pstrdup("pg_qx_runtime");
	request.command_line = psprintf("%s -M microvm -accel %s -kernel %s -initrd %s",
									qemu_path,
									accel,
									kernel_path,
									initrd_path);
	request.kernel_ref = pstrdup(kernel_path);
	request.initrd_ref = pstrdup(initrd_path);
	request.image_ref = image_ref;
	request.snapshot_ref = NULL;
	request.receipt_schema = pstrdup(qx_safe_runtime_text(receipt_schema));
	request.receipt_alg = pstrdup(qx_safe_runtime_text(receipt_alg));
	request.receipt_nonce = pstrdup(qx_safe_runtime_text(receipt_nonce));
	request.attestation_mode =
		pstrdup(qx_microvm_backend_expected_attestation_mode());
	request.detail = psprintf("task=%u;handler=%s",
							  taskoid,
							  handler_name != NULL ? handler_name : "<unknown>");
	request.timeout_ms = profile->timeout_ms;
	request.memory_kb = profile->memory_kb;
	request.process_limit = profile->process_limit;
	request.vcpu_count = 1;
	request.token_charge = 0;
	request.cost_charge = 0;
	request.require_attestation = require_attestation;
	request.allow_network = false;
	request.allow_privilege_escalation = false;

	qx_microvm_backend_validate_request(&request);
	qx_microvm_backend_request_free(&request);
	pfree(accel);
}

static char *
qx_provider_receipt_key(Oid provideroid)
{
	QxCatalogProviderInfo provider;
	char	   *receipt_key;

	if (!QxCatalogLookupProviderByOid(provideroid, &provider))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("provider %u no longer exists for runtime receipt verification",
						provideroid)));

	if (!provider.enabled)
	{
		QxCatalogFreeProviderInfo(&provider);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("provider %u is disabled for runtime receipt verification",
						provideroid)));
	}

	if (provider.receipt_key == NULL || provider.receipt_key[0] == '\0')
	{
		QxCatalogFreeProviderInfo(&provider);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("provider %u has no receipt key configured", provideroid)));
	}

	receipt_key = pstrdup(provider.receipt_key);
	QxCatalogFreeProviderInfo(&provider);

	return receipt_key;
}

static const char *
qx_default_runtime_for_provider_kind(const char *provider_kind)
{
	if (provider_kind != NULL && strcmp(provider_kind, "container") == 0)
		return "container";
	if (provider_kind != NULL && strcmp(provider_kind, "microvm") == 0)
		return "microvm";

	return "host";
}

static char *
qx_expected_attestation_mode(const char *provider_kind, bool require_attestation)
{
	if (!require_attestation)
		return pstrdup("optional");

	if (provider_kind != NULL && strcmp(provider_kind, "microvm") == 0)
		return pstrdup("microvm_receipt_verified");

	if (provider_kind != NULL && strcmp(provider_kind, "container") == 0)
		return pstrdup("container_receipt_verified");

	if (provider_kind != NULL && strcmp(provider_kind, "remote") == 0)
		return pstrdup("remote_broker_verified");

	return pstrdup("loopback_verified");
}

static char *
qx_receipt_hmac_signature_hex(const char *receipt_key, const char *payload)
{
	pg_hmac_ctx *ctx;
	uint8		digest[PG_SHA256_DIGEST_LENGTH];
	char	   *hex;
	int			i;

	ctx = pg_hmac_create(PG_SHA256);
	if (ctx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not allocate receipt verifier context")));

	if (pg_hmac_init(ctx, (const uint8 *) receipt_key, strlen(receipt_key)) < 0 ||
		pg_hmac_update(ctx, (const uint8 *) payload, strlen(payload)) < 0 ||
		pg_hmac_final(ctx, digest, sizeof(digest)) < 0)
	{
		const char *reason = pg_hmac_error(ctx);

		pg_hmac_free(ctx);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not compute provider receipt signature"),
				 errdetail("%s", reason)));
	}

	pg_hmac_free(ctx);
	hex = palloc(QX_RECEIPT_SIG_HEX_LEN);
	for (i = 0; i < PG_SHA256_DIGEST_LENGTH; i++)
		snprintf(hex + (i * 2), 3, "%02x", digest[i]);
	hex[QX_RECEIPT_SIG_HEX_LEN - 1] = '\0';
	explicit_bzero(digest, sizeof(digest));

	return hex;
}

#ifdef USE_OPENSSL
static char *
qx_openssl_error_string(void)
{
	unsigned long errcode;
	char		buffer[256];

	errcode = ERR_get_error();
	if (errcode == 0)
		return pstrdup("no OpenSSL error reported");

	ERR_error_string_n(errcode, buffer, sizeof(buffer));
	return pstrdup(buffer);
}

static int
qx_hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool
qx_hex_decode_signature(const char *hex, uint8 *dest, size_t expected_len)
{
	size_t		hexlen;
	size_t		i;

	if (hex == NULL)
		return false;

	hexlen = strlen(hex);
	if (hexlen != expected_len * 2)
		return false;

	for (i = 0; i < expected_len; i++)
	{
		int			hi = qx_hex_value(hex[i * 2]);
		int			lo = qx_hex_value(hex[(i * 2) + 1]);

		if (hi < 0 || lo < 0)
			return false;
		dest[i] = (uint8) ((hi << 4) | lo);
	}

	return true;
}

static bool
qx_verify_ed25519_receipt_signature(const char *public_key_pem,
									  const char *payload,
									  const char *signature_hex)
{
	BIO		   *bio = NULL;
	EVP_PKEY   *pkey = NULL;
	EVP_MD_CTX *mdctx = NULL;
	uint8		signature[QX_ED25519_SIG_BYTES];
	bool		verified = false;
	int			rc;

	if (public_key_pem == NULL || public_key_pem[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("provider has no public key configured for ed25519 receipt verification")));

	if (!qx_hex_decode_signature(signature_hex, signature, sizeof(signature)))
		return false;

	bio = BIO_new_mem_buf(public_key_pem, -1);
	if (bio == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not allocate receipt public-key buffer")));

	pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
	if (pkey == NULL)
	{
		char	   *detail = qx_openssl_error_string();

		BIO_free(bio);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not load ed25519 receipt public key"),
				 errdetail("%s", detail)));
	}

	mdctx = EVP_MD_CTX_new();
	if (mdctx == NULL)
	{
		BIO_free(bio);
		EVP_PKEY_free(pkey);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not allocate ed25519 verifier context")));
	}

	rc = EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, pkey);
	if (rc != 1)
	{
		char	   *detail = qx_openssl_error_string();

		EVP_MD_CTX_free(mdctx);
		BIO_free(bio);
		EVP_PKEY_free(pkey);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not initialize ed25519 receipt verifier"),
				 errdetail("%s", detail)));
	}

	rc = EVP_DigestVerify(mdctx,
						  signature, sizeof(signature),
						  (const unsigned char *) payload, strlen(payload));
	verified = (rc == 1);

	EVP_MD_CTX_free(mdctx);
	BIO_free(bio);
	EVP_PKEY_free(pkey);
	explicit_bzero(signature, sizeof(signature));

	return verified;
}
#endif

static char *
qx_receipt_payload(const char *phase,
				   Oid taskoid,
				   const char *tool_name,
				   const char *principal_name,
				   const char *principal_runtime,
				   const char *provider_name,
				   const char *provider_kind,
				   const char *provider_endpoint,
				   const char *sandbox_name,
				   const char *profile_name,
				   const char *environment_mode,
				   const char *workdir_name,
				   int32 timeout_ms,
				   int32 process_limit,
				   bool path_present,
				   const char *receipt_schema,
				   const char *receipt_alg,
				   const char *receipt_nonce,
				   const char *attestation_mode,
				   int32 token_charge,
				   int32 cost_charge,
				   const char *detail)
{
	return psprintf("phase=%s;task=%u;tool=%s;principal=%s;principal_runtime=%s;provider=%s;provider_kind=%s;provider_endpoint=%s;sandbox=%s;profile=%s;env=%s;workdir=%s;timeout_ms=%d;process_limit=%d;path_present=%s;receipt_schema=%s;receipt_alg=%s;receipt_nonce=%s;attestation=%s;tokens=%d;cost=%d;detail=%s",
					phase != NULL ? phase : "submit",
					taskoid,
					tool_name != NULL ? tool_name : "<unknown>",
					principal_name != NULL ? principal_name : "<unknown>",
					principal_runtime != NULL ? principal_runtime : "host",
					provider_name != NULL ? provider_name : "<unknown>",
					provider_kind != NULL ? provider_kind : "loopback",
					provider_endpoint != NULL ? provider_endpoint : "local://qhapaqxian-tool-runner",
					sandbox_name != NULL ? sandbox_name : "builtin",
					profile_name != NULL ? profile_name : "builtin",
					environment_mode != NULL ? environment_mode : "minimal",
					workdir_name != NULL ? workdir_name : "pg_qx_runtime",
					timeout_ms,
					process_limit,
					path_present ? "true" : "false",
					receipt_schema != NULL ? receipt_schema : "qx.receipt.v1",
					receipt_alg != NULL ? receipt_alg : "hmac-sha256",
					receipt_nonce != NULL ? receipt_nonce : "<unknown>",
					attestation_mode != NULL ? attestation_mode : "optional",
					token_charge,
					cost_charge,
					detail != NULL ? detail : "<none>");
}

static int
qx_sandbox_rank(const char *sandbox_name)
{
	if (sandbox_name == NULL || strcmp(sandbox_name, "builtin") == 0)
		return 0;
	if (strcmp(sandbox_name, "restricted") == 0)
		return 1;
	if (strcmp(sandbox_name, "isolated") == 0)
		return 2;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unsupported sandbox \"%s\"", sandbox_name)));
	return -1;
}

static const char *
qx_effective_sandbox_name(const char *tool_sandbox,
						  const char *principal_sandbox)
{
	if (qx_sandbox_rank(principal_sandbox) >= qx_sandbox_rank(tool_sandbox))
		return principal_sandbox != NULL ? principal_sandbox : "builtin";

	return tool_sandbox != NULL ? tool_sandbox : "builtin";
}

static const QxSandboxProfile *
qx_lookup_sandbox_profile(const char *sandbox_name)
{
	static const QxSandboxProfile builtin_profile = {
		"builtin", 3000, 262144, 1024, 64, 2
	};
	static const QxSandboxProfile restricted_profile = {
		"restricted", 2000, 131072, 256, 32, 1
	};
	static const QxSandboxProfile isolated_profile = {
		"isolated", 1200, 65536, 64, 16, 1
	};

	if (sandbox_name == NULL || strcmp(sandbox_name, "builtin") == 0)
		return &builtin_profile;
	if (strcmp(sandbox_name, "restricted") == 0)
		return &restricted_profile;
	if (strcmp(sandbox_name, "isolated") == 0)
		return &isolated_profile;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unsupported sandbox profile \"%s\"", sandbox_name)));
	return NULL;
}

static void
qx_runtime_temp_dir(char *path, size_t pathlen)
{
	char		dir[MAXPGPATH];

	join_path_components(dir, DataDir, "pg_qx_runtime");
	if (MakePGDirectory(dir) < 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create QhapaqXian runtime directory \"%s\": %m",
						dir)));

	strlcpy(path, dir, pathlen);
}

static void
qx_runtime_temp_path(char *path, size_t pathlen, const char *suffix)
{
	char		dir[MAXPGPATH];
	char		filename[MAXPGPATH];

	qx_runtime_temp_dir(dir, sizeof(dir));
	qx_runtime_temp_seq++;
	snprintf(filename, sizeof(filename), "qx_tool_%d_%lld_%u.%s",
			 MyProcPid, (long long) GetCurrentTimestamp(),
			 qx_runtime_temp_seq, suffix);
	join_path_components(path, dir, filename);
}

static void
qx_write_text_file(const char *path, const char *contents)
{
	FILE	   *file;

	file = AllocateFile(path, "w");
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open runtime file \"%s\": %m", path)));

	if (fputs(contents, file) < 0)
	{
		FreeFile(file);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write runtime file \"%s\": %m", path)));
	}

	FreeFile(file);
}

static void
qx_resolve_docker_cli_path(char *resolved, size_t resolved_len)
{
	const char *override;

	Assert(resolved != NULL);
	override = getenv("QX_DOCKER_CLI");
	if (override != NULL && override[0] != '\0')
	{
		strlcpy(resolved, override, resolved_len);
		return;
	}

#ifdef WIN32
	if (SearchPathA(NULL, "docker.exe", NULL, (DWORD) resolved_len,
					resolved, NULL) > 0)
		return;

	strlcpy(resolved,
			"C:\\Program Files\\Docker\\Docker\\resources\\bin\\docker.exe",
			resolved_len);
#else
	{
		const char *path = getenv("PATH");

		if (path != NULL && path[0] != '\0')
		{
			char	   *copy = pstrdup(path);
			char	   *cursor = copy;

			while (cursor != NULL && *cursor != '\0')
			{
				char	   *sep = strchr(cursor, ':');
				char		candidate[MAXPGPATH];

				if (sep != NULL)
					*sep = '\0';
				join_path_components(candidate, cursor, "docker");
				if (access(candidate, X_OK) == 0)
				{
					strlcpy(resolved, candidate, resolved_len);
					pfree(copy);
					return;
				}
				cursor = sep != NULL ? sep + 1 : NULL;
			}
			pfree(copy);
		}
	}

	strlcpy(resolved, "/usr/bin/docker", resolved_len);
#endif
}

static bool
qx_runtime_path_exists(const char *path)
{
	struct stat st;

	if (path == NULL || path[0] == '\0')
		return false;

	return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static bool
qx_resolve_microvm_asset_from_builddir(const char *filename,
										 char *resolved,
										 size_t resolved_len)
{
	const char *top_builddir;
	char		asset_dir[MAXPGPATH];
	char		candidate[MAXPGPATH];

	top_builddir = getenv("top_builddir");
	if (top_builddir == NULL || top_builddir[0] == '\0')
		return false;

	join_path_components(asset_dir, top_builddir, "microvm-assets");
	join_path_components(candidate, asset_dir, filename);
	canonicalize_path(candidate);
	if (!qx_runtime_path_exists(candidate))
		return false;

	strlcpy(resolved, candidate, resolved_len);
	return true;
}

static bool
qx_resolve_microvm_runtime_paths(char *kernel_path,
								 size_t kernel_path_len,
								 char *initrd_path,
								 size_t initrd_path_len)
{
	const char *kernel_override = getenv("QX_MICROVM_KERNEL");
	const char *initrd_override = getenv("QX_MICROVM_INITRD");

	if (kernel_override != NULL && kernel_override[0] != '\0' &&
		initrd_override != NULL && initrd_override[0] != '\0' &&
		qx_runtime_path_exists(kernel_override) &&
		qx_runtime_path_exists(initrd_override))
	{
		strlcpy(kernel_path, kernel_override, kernel_path_len);
		strlcpy(initrd_path, initrd_override, initrd_path_len);
		return true;
	}

	if (!qx_resolve_microvm_asset_from_builddir("vmlinuz-virt",
												 kernel_path,
												 kernel_path_len))
		return false;

	if (qx_resolve_microvm_asset_from_builddir("initramfs-qx-microvm.cpio.gz",
												 initrd_path,
												 initrd_path_len))
		return true;

	if (qx_resolve_microvm_asset_from_builddir("initramfs-qx-microvm2.cpio.gz",
												 initrd_path,
												 initrd_path_len))
		return true;

	kernel_path[0] = '\0';
	return false;
}

static void
qx_resolve_microvm_qemu_path(char *resolved, size_t resolved_len)
{
	const char *override = getenv("QX_MICROVM_QEMU");

	Assert(resolved != NULL);

	if (override != NULL && override[0] != '\0')
	{
		strlcpy(resolved, override, resolved_len);
		return;
	}

#ifdef WIN32
	if (SearchPathA(NULL, "qemu-system-x86_64.exe", NULL, (DWORD) resolved_len,
					resolved, NULL) > 0)
		return;

	strlcpy(resolved,
			"C:\\Program Files\\qemu\\qemu-system-x86_64.exe",
			resolved_len);
#else
	strlcpy(resolved, "qemu-system-x86_64", resolved_len);
#endif
}

static const char *
qx_default_docker_host(void)
{
	const char *override = getenv("DOCKER_HOST");

	if (override != NULL && override[0] != '\0')
		return override;

#ifdef WIN32
	return "npipe:////./pipe/dockerDesktopLinuxEngine";
#else
	return "unix:///var/run/docker.sock";
#endif
}

static void
qx_resolve_principal_program_path(const char *program_name,
								  char *resolved,
								  size_t resolved_len)
{
	char		bindir[MAXPGPATH];
	char		candidate[MAXPGPATH];
	char		program_with_ext[MAXPGPATH];

	if (program_name == NULL || program_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("principal program is not configured")));

	strlcpy(bindir, my_exec_path, sizeof(bindir));
	get_parent_directory(bindir);
	canonicalize_path(bindir);

	if (is_absolute_path(program_name))
		strlcpy(candidate, program_name, sizeof(candidate));
	else
	{
		strlcpy(program_with_ext, program_name, sizeof(program_with_ext));
#if defined(WIN32) && !defined(__CYGWIN__)
		if (strchr(program_with_ext, '.') == NULL)
			strlcat(program_with_ext, EXE, sizeof(program_with_ext));
#endif
		join_path_components(candidate, bindir, program_with_ext);
	}

	canonicalize_path(candidate);
	if (!path_is_prefix_of_path(bindir, candidate))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("principal program \"%s\" resolves outside the server bindir",
						program_name),
				 errdetail("Only executables shipped with the QhapaqXian installation are allowed for external tool principals.")));

	if (validate_exec(candidate) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("principal program \"%s\" is not executable", candidate)));

	strlcpy(resolved, candidate, resolved_len);
}

static void
qx_resolve_receipt_signer_path(const char *signer_name,
							   char *resolved,
							   size_t resolved_len)
{
	char		bindir[MAXPGPATH];
	char		candidate[MAXPGPATH];
	struct stat st;

	if (signer_name == NULL || signer_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("principal receipt signer is not configured")));

	strlcpy(bindir, my_exec_path, sizeof(bindir));
	get_parent_directory(bindir);
	canonicalize_path(bindir);

	if (is_absolute_path(signer_name))
		strlcpy(candidate, signer_name, sizeof(candidate));
	else
		join_path_components(candidate, bindir, signer_name);

	canonicalize_path(candidate);
	if (!path_is_prefix_of_path(bindir, candidate))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("receipt signer \"%s\" resolves outside the server bindir",
						signer_name),
				 errdetail("Only signer files shipped with the QhapaqXian installation are allowed for asymmetric receipts.")));

	if (stat(candidate, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("receipt signer \"%s\" is not readable", candidate)));

#ifndef WIN32
	if (S_ISDIR(st.st_mode))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("receipt signer \"%s\" is not a file", candidate)));
#endif

	strlcpy(resolved, candidate, resolved_len);
}

static void
qx_launch_principal_program(const char *program_path,
							const QxSandboxProfile *profile,
							const char *runtime_dir,
							const char *request_path,
							const char *response_path,
							const char *launch_request_path,
							bool preserve_host_identity,
							QxSandboxObservation *observation)
{
#ifndef WIN32
	pid_t		pid;
	int			status;
	bool		timed_out = false;
	TimestampTz	started_at;
	char	   *argv[6];
	char	   *envp[10];
	int			env_index = 0;

	argv[0] = unconstify(char *, program_path);
	argv[1] = "--request-file";
	argv[2] = unconstify(char *, request_path);
	argv[3] = "--response-file";
	argv[4] = unconstify(char *, response_path);
	argv[5] = NULL;
	observation->launch_mode = "profiled_process";
	observation->restricted_identity = false;
	observation->wall_time_ms = 0;

	envp[env_index++] = psprintf("QX_SANDBOX_PROFILE=%s", profile->name);
	envp[env_index++] = psprintf("QX_SANDBOX_TIMEOUT_MS=%d", profile->timeout_ms);
	envp[env_index++] = psprintf("QX_SANDBOX_MEMORY_KB=%d", profile->memory_kb);
	envp[env_index++] = psprintf("QX_SANDBOX_PROCESS_LIMIT=%d", profile->process_limit);
	envp[env_index++] = psprintf("QX_SANDBOX_MAX_OPEN_FILES=%d", profile->max_open_files);
	envp[env_index++] = psprintf("QX_RUNTIME_ROOT=%s", runtime_dir);
	envp[env_index++] = psprintf("TMPDIR=%s", runtime_dir);
	if (launch_request_path != NULL && launch_request_path[0] != '\0')
		envp[env_index++] = psprintf("LAUNCH_REQUEST_FILE=%s",
									 launch_request_path);
	envp[env_index++] = pstrdup("LANG=C");
	envp[env_index] = NULL;

	pid = fork();
	if (pid < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fork principal program \"%s\": %m",
						program_path)));

	if (pid == 0)
	{
		long		maxfd;
		int			fd;

		if (chdir(runtime_dir) != 0)
			_exit(126);

#ifdef RLIMIT_NOFILE
		{
			struct rlimit limit;

			limit.rlim_cur = profile->max_open_files;
			limit.rlim_max = profile->max_open_files;
			(void) setrlimit(RLIMIT_NOFILE, &limit);
		}
#endif
#ifdef RLIMIT_FSIZE
		if (!preserve_host_identity)
		{
			struct rlimit limit;

			limit.rlim_cur = (rlim_t) profile->file_kb * 1024;
			limit.rlim_max = (rlim_t) profile->file_kb * 1024;
			(void) setrlimit(RLIMIT_FSIZE, &limit);
		}
#endif
#ifdef RLIMIT_AS
		if (!preserve_host_identity)
		{
			struct rlimit limit;

			limit.rlim_cur = (rlim_t) profile->memory_kb * 1024;
			limit.rlim_max = (rlim_t) profile->memory_kb * 1024;
			(void) setrlimit(RLIMIT_AS, &limit);
		}
#endif
#ifdef RLIMIT_CPU
		if (!preserve_host_identity)
		{
			struct rlimit limit;
			int32		cpu_seconds;

			cpu_seconds = (profile->timeout_ms + 999) / 1000;
			if (cpu_seconds <= 0)
				cpu_seconds = 1;
			limit.rlim_cur = cpu_seconds;
			limit.rlim_max = cpu_seconds;
			(void) setrlimit(RLIMIT_CPU, &limit);
		}
#endif

		maxfd = sysconf(_SC_OPEN_MAX);
		if (maxfd < 0 || maxfd > 1024)
			maxfd = 1024;
		for (fd = 3; fd < maxfd; fd++)
			close(fd);

		execve(program_path, argv, envp);
		_exit(127);
	}

	started_at = GetCurrentTimestamp();
	for (;;)
	{
		pid_t		wait_result;

		wait_result = waitpid(pid, &status, WNOHANG);
		if (wait_result == pid)
			break;
		if (wait_result < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not wait for principal program \"%s\": %m",
							program_path)));
		if (TimestampDifferenceExceeds(started_at, GetCurrentTimestamp(),
									   profile->timeout_ms))
		{
			(void) kill(pid, SIGKILL);
			(void) waitpid(pid, &status, 0);
			timed_out = true;
			break;
		}

		pg_usleep(10000L);
	}

	while (env_index > 0)
		pfree(envp[--env_index]);
	observation->wall_time_ms =
		(int32) TimestampDifferenceMilliseconds(started_at, GetCurrentTimestamp());

	if (timed_out)
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("principal program \"%s\" exceeded sandbox timeout",
						program_path),
				 errdetail("Sandbox profile \"%s\" allows at most %d ms of wall time.",
						   profile->name, profile->timeout_ms)));

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal program \"%s\" failed", program_path),
				 errdetail("Exit status was %d.", WIFEXITED(status) ? WEXITSTATUS(status) : -1)));
#else
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits;
	HANDLE		job;
	HANDLE		base_token = NULL;
	HANDLE		launch_token = NULL;
	StringInfoData environment;
	char		cmdline[(MAXPGPATH * 3) + 128];
	DWORD		exit_code = 0;
	DWORD		wait_status;
	DWORD		creation_flags;
	DWORD		active_process_limit;
	DWORD		job_limit_flags;
	SIZE_T		process_memory_limit;
	const char *system_root = getenv("SystemRoot");
	char	   *timeout_value;
	char	   *memory_value;
	char	   *process_limit_value;
	char	   *nofile_value;
	bool		need_restricted_identity;
	TimestampTz	started_at;

	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	ZeroMemory(&job_limits, sizeof(job_limits));
	si.cb = sizeof(si);
	observation->launch_mode = "profiled_process";
	observation->restricted_identity = false;
	observation->wall_time_ms = 0;
	/*
	 * When the runtime delegates isolation to the container backend on
	 * Windows, preserve the host identity so Docker Desktop's brokered named
	 * pipe remains reachable. The job object still constrains the launcher,
	 * but it must allow the local Docker broker process to spawn.
	 */
	need_restricted_identity =
		(!preserve_host_identity && strcmp(profile->name, "builtin") != 0);
	active_process_limit = (DWORD) profile->process_limit;
	process_memory_limit = (SIZE_T) profile->memory_kb * 1024;
	if (preserve_host_identity)
	{
		if (active_process_limit < 8)
			active_process_limit = 8;
		process_memory_limit = 0;
	}

	job = NULL;
	if (!preserve_host_identity)
	{
		job = CreateJobObjectA(NULL, NULL);
		if (job == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create sandbox job object for principal \"%s\": %m (error code %lu)",
							program_path, GetLastError())));

		job_limit_flags =
			JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
			JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
			JOB_OBJECT_LIMIT_PROCESS_MEMORY;
		job_limits.BasicLimitInformation.LimitFlags = job_limit_flags;
		job_limits.BasicLimitInformation.ActiveProcessLimit = active_process_limit;
		job_limits.ProcessMemoryLimit = process_memory_limit;
		if (!SetInformationJobObject(job,
									 JobObjectExtendedLimitInformation,
									 &job_limits,
									 sizeof(job_limits)))
		{
			CloseHandle(job);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not configure sandbox job object for principal \"%s\": %m (error code %lu)",
							program_path, GetLastError())));
		}
	}

	timeout_value = psprintf("%d", profile->timeout_ms);
	memory_value = psprintf("%d", profile->memory_kb);
	process_limit_value = psprintf("%d", profile->process_limit);
	nofile_value = psprintf("%d", profile->max_open_files);
	initStringInfo(&environment);
#define QX_APPEND_ENV(name, value) \
	do { \
		appendStringInfo(&environment, "%s=%s", (name), (value)); \
		appendBinaryStringInfo(&environment, "\0", 1); \
	} while (0)
	QX_APPEND_ENV("QX_SANDBOX_PROFILE", profile->name);
	QX_APPEND_ENV("QX_SANDBOX_TIMEOUT_MS", timeout_value);
	QX_APPEND_ENV("QX_SANDBOX_MEMORY_KB", memory_value);
	QX_APPEND_ENV("QX_SANDBOX_PROCESS_LIMIT", process_limit_value);
	QX_APPEND_ENV("QX_SANDBOX_MAX_OPEN_FILES", nofile_value);
	QX_APPEND_ENV("QX_RUNTIME_ROOT", runtime_dir);
	QX_APPEND_ENV("TMP", runtime_dir);
	QX_APPEND_ENV("TEMP", runtime_dir);
	if (launch_request_path != NULL && launch_request_path[0] != '\0')
		QX_APPEND_ENV("LAUNCH_REQUEST_FILE", launch_request_path);
	if (system_root != NULL && system_root[0] != '\0')
		QX_APPEND_ENV("SystemRoot", system_root);
	appendBinaryStringInfo(&environment, "\0", 1);
#undef QX_APPEND_ENV
	pfree(timeout_value);
	pfree(memory_value);
	pfree(process_limit_value);
	pfree(nofile_value);

	snprintf(cmdline, sizeof(cmdline),
			 "\"%s\" --request-file \"%s\" --response-file \"%s\"",
			 program_path, request_path, response_path);
	creation_flags = CREATE_NO_WINDOW;
	if (!preserve_host_identity)
		creation_flags |= CREATE_SUSPENDED;

	if (need_restricted_identity)
	{
		if (!OpenProcessToken(GetCurrentProcess(),
							  TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
							  TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
							  TOKEN_ADJUST_PRIVILEGES,
							  &base_token))
		{
			if (job != NULL)
				CloseHandle(job);
			pfree(environment.data);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open process token for principal \"%s\": %m (error code %lu)",
							program_path, GetLastError())));
		}

		if (!CreateRestrictedToken(base_token,
								   DISABLE_MAX_PRIVILEGE | LUA_TOKEN,
								   0, NULL, 0, NULL, 0, NULL,
								   &launch_token))
		{
			CloseHandle(base_token);
			if (job != NULL)
				CloseHandle(job);
			pfree(environment.data);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create restricted token for principal \"%s\": %m (error code %lu)",
							program_path, GetLastError())));
		}
	}

	started_at = GetCurrentTimestamp();
	if (!(need_restricted_identity ?
		  CreateProcessAsUserA(launch_token,
							   program_path,
							   cmdline,
							   NULL,
							   NULL,
							   FALSE,
							   creation_flags,
							   environment.data,
							   runtime_dir,
							   &si,
							   &pi) :
		  CreateProcessA(program_path,
						 cmdline,
						 NULL,
						 NULL,
						 FALSE,
						 creation_flags,
						 environment.data,
						 runtime_dir,
						 &si,
						 &pi)))
	{
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		if (job != NULL)
			CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not start principal program \"%s\": %m (error code %lu)",
						program_path, GetLastError())));
	}

	if (job != NULL && !AssignProcessToJobObject(job, pi.hProcess))
	{
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not assign principal program \"%s\" to sandbox job: %m (error code %lu)",
						program_path, GetLastError())));
	}

	if (!preserve_host_identity &&
		ResumeThread(pi.hThread) == (DWORD) -1)
	{
		if (job != NULL)
			TerminateJobObject(job, 1);
		else
			TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		if (job != NULL)
			CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not resume principal program \"%s\": %m (error code %lu)",
						program_path, GetLastError())));
	}

	wait_status = WaitForSingleObject(pi.hProcess, profile->timeout_ms);
	if (wait_status == WAIT_TIMEOUT)
	{
		if (job != NULL)
			TerminateJobObject(job, 1);
		else
			TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, INFINITE);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		if (job != NULL)
			CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("principal program \"%s\" exceeded sandbox timeout",
						program_path),
				 errdetail("Sandbox profile \"%s\" allows at most %d ms of wall time.",
						   profile->name, profile->timeout_ms)));
	}

	if (wait_status != WAIT_OBJECT_0)
	{
		if (job != NULL)
			TerminateJobObject(job, 1);
		else
			TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		if (job != NULL)
			CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not wait for principal program \"%s\": %m (error code %lu)",
						program_path, GetLastError())));
	}

	if (!GetExitCodeProcess(pi.hProcess, &exit_code))
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		if (job != NULL)
			CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not get exit status for principal program \"%s\": %m (error code %lu)",
						program_path, GetLastError())));
	}

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	if (launch_token != NULL)
		CloseHandle(launch_token);
	if (base_token != NULL)
		CloseHandle(base_token);
	if (job != NULL)
		CloseHandle(job);
	observation->wall_time_ms =
		(int32) TimestampDifferenceMilliseconds(started_at, GetCurrentTimestamp());
	observation->restricted_identity = need_restricted_identity;
	pfree(environment.data);

	if (exit_code != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal program \"%s\" failed", program_path),
				 errdetail("Exit status was %lu.", exit_code)));
#endif
}

static void
qx_read_external_result(const char *path, QxExternalToolResult *result)
{
	FILE	   *file;
	char		line[8192];

	file = AllocateFile(path, "r");
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read principal response file \"%s\": %m",
						path)));

	memset(result, 0, sizeof(*result));
	while (fgets(line, sizeof(line), file) != NULL)
	{
		char	   *eq;
		char	   *key;
		char	   *value;
		size_t		value_len;
		bool		partial_line;

		eq = strchr(line, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		key = line;
		value = eq + 1;
		value_len = strcspn(value, "\r\n");
		value[value_len] = '\0';
		partial_line = (value_len == sizeof(line) - (value - line) - 1 &&
						strchr(value, '\n') == NULL &&
						!feof(file));

		if (strcmp(key, "TOKENS") == 0)
			result->token_charge = pg_strtoint32(value);
		else if (strcmp(key, "COST") == 0)
			result->cost_charge = pg_strtoint32(value);
		else if (strcmp(key, "DETAIL") == 0)
		{
			StringInfoData detail_buf;

			initStringInfo(&detail_buf);
			appendStringInfoString(&detail_buf, value);
			while (partial_line && fgets(line, sizeof(line), file) != NULL)
			{
				char	   *newline = strchr(line, '\n');

				if (newline != NULL)
					*newline = '\0';
				appendStringInfoString(&detail_buf, line);
				partial_line = (newline == NULL);
			}
			result->detail = detail_buf.data;
		}
		else if (strcmp(key, "TOOL") == 0)
			result->tool_name = pstrdup(value);
		else if (strcmp(key, "PRINCIPAL") == 0)
			result->principal_name = pstrdup(value);
		else if (strcmp(key, "PRINCIPAL_RUNTIME") == 0)
			result->principal_runtime = pstrdup(value);
		else if (strcmp(key, "PROVIDER") == 0)
			result->provider_name = pstrdup(value);
		else if (strcmp(key, "PROVIDER_KIND") == 0)
			result->provider_kind = pstrdup(value);
		else if (strcmp(key, "SANDBOX") == 0)
			result->sandbox_name = pstrdup(value);
		else if (strcmp(key, "PROFILE") == 0)
			result->profile_name = pstrdup(value);
		else if (strcmp(key, "ENV") == 0)
			result->environment_mode = pstrdup(value);
		else if (strcmp(key, "WORKDIR") == 0)
			result->workdir_name = pstrdup(value);
		else if (strcmp(key, "RECEIPT_SCHEMA") == 0)
			result->receipt_schema = pstrdup(value);
		else if (strcmp(key, "RECEIPT_ALG") == 0)
			result->receipt_alg = pstrdup(value);
		else if (strcmp(key, "RECEIPT_NONCE") == 0)
			result->receipt_nonce = pstrdup(value);
		else if (strcmp(key, "RECEIPT_SIG") == 0)
			result->receipt_signature = pstrdup(value);
		else if (strcmp(key, "ATTESTATION") == 0)
			result->attestation_mode = pstrdup(value);
		else if (strcmp(key, "TIMEOUT_MS") == 0)
			result->timeout_ms = pg_strtoint32(value);
		else if (strcmp(key, "PROCESS_LIMIT") == 0)
			result->process_limit = pg_strtoint32(value);
		else if (strcmp(key, "PATH_PRESENT") == 0)
			result->path_present = (strcmp(value, "true") == 0);
		else if (strcmp(key, "CONTAINER_ID") == 0)
			result->container_id = pstrdup(value);
		else if (strcmp(key, "VM_ID") == 0)
			result->vm_id = pstrdup(value);
		else if (strcmp(key, "STATUS") == 0 && strcmp(value, "ok") != 0)
		{
			FreeFile(file);
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("principal response reported status \"%s\"", value)));
		}
	}

	FreeFile(file);

	if (result->detail == NULL)
		result->detail = pstrdup("external tool execution completed");
}

static char *
qx_strip_duplicate_receipt_tail_once(const char *source)
{
	const char *cursor;
	const char *duplicate;

	if (source == NULL)
		return NULL;

	cursor = strstr(source, ";detail=");
	if (cursor != NULL)
	{
		duplicate = strstr(cursor + strlen(";detail="), ";detail=");
		if (duplicate != NULL)
			return pnstrdup(source, duplicate - source);
	}

	cursor = strstr(source, ";tokens=");
	if (cursor != NULL)
	{
		duplicate = strstr(cursor + strlen(";tokens="), ";tokens=");
		if (duplicate != NULL)
			return pnstrdup(source, duplicate - source);
	}

	cursor = strstr(source, ";launch_mode=");
	if (cursor != NULL)
	{
		duplicate = strstr(cursor + strlen(";launch_mode="), ";launch_mode=");
		if (duplicate != NULL)
			return pnstrdup(source, duplicate - source);
	}

	return NULL;
}

static char *
qx_strip_duplicate_receipt_tail(const char *source)
{
	char	   *stripped;
	char	   *next;

	if (source == NULL)
		return NULL;

	stripped = pstrdup(source);
	for (;;)
	{
		next = qx_strip_duplicate_receipt_tail_once(stripped);
		if (next == NULL)
			break;
		pfree(stripped);
		stripped = next;
	}

	return stripped;
}

static char *
qx_truncate_detail_after_wall_ms(const char *source)
{
	const char *wall_ms;
	const char *cursor;

	if (source == NULL)
		return NULL;

	wall_ms = strstr(source, ";wall_ms=");
	if (wall_ms == NULL)
		return pstrdup(source);

	cursor = wall_ms + strlen(";wall_ms=");
	while (*cursor >= '0' && *cursor <= '9')
		cursor++;

	return pnstrdup(source, cursor - source);
}

static char *
qx_strip_external_trace_payload(const char *source)
{
	char	   *stripped;
	const char *detail_marker;

	if (source == NULL)
		return NULL;

	stripped = qx_strip_duplicate_receipt_tail(source);
	detail_marker = strstr(stripped, ";detail=");
	if (detail_marker != NULL)
	{
		const char *inner_detail = detail_marker + strlen(";detail=");
		const char *tokens_in_inner = strstr(inner_detail, ";tokens=");
		const char *detail_dup = strstr(inner_detail, ";detail=");
		const char *truncate_at = NULL;
		char	   *clean_inner;
		char	   *rewritten;

		if (tokens_in_inner != NULL &&
			(detail_dup == NULL || tokens_in_inner < detail_dup))
			truncate_at = tokens_in_inner;
		else if (detail_dup != NULL)
			truncate_at = detail_dup;

		if (truncate_at != NULL)
		{
			rewritten = pnstrdup(stripped, truncate_at - stripped);
			pfree(stripped);
			stripped = rewritten;
		}
		else
		{
			clean_inner = qx_truncate_detail_after_wall_ms(inner_detail);
			if (strcmp(clean_inner, inner_detail) != 0)
			{
				rewritten = psprintf("%.*s%s",
									 (int) (inner_detail - stripped),
									 stripped,
									 clean_inner);
				pfree(stripped);
				stripped = rewritten;
			}
			pfree(clean_inner);
		}
	}

	return stripped;
}

static char *
qx_format_external_execution_payload(const char *phase,
									 const QxExternalToolResult *result)
{
	char	   *payload;
	char	   *stripped;

	{
		char	   *detail_for_payload;
		const char *tokens_in_detail;

		detail_for_payload = qx_strip_duplicate_receipt_tail(result->detail);
		{
			char	   *truncated_detail;

			tokens_in_detail = strstr(detail_for_payload, ";tokens=");
			if (tokens_in_detail != NULL)
			{
				truncated_detail = pnstrdup(detail_for_payload,
											tokens_in_detail - detail_for_payload);
				pfree(detail_for_payload);
				detail_for_payload = truncated_detail;
			}
			truncated_detail = qx_truncate_detail_after_wall_ms(detail_for_payload);
			pfree(detail_for_payload);
			detail_for_payload = truncated_detail;
		}
		payload = psprintf("phase=%s;tool=%s;principal=%s;principal_runtime=%s;provider=%s;provider_kind=%s;effective_sandbox=%s;profile=%s;env=%s;cwd=%s;process_limit=%d;timeout_ms=%d;receipt_schema=%s;receipt_alg=%s;receipt_nonce=%s;receipt_sig=%s;attestation=%s;container_id=%s;vm_id=%s;tokens=%d;cost=%d;detail=%s",
							phase != NULL ? phase : "submit",
							result->tool_name != NULL ? result->tool_name : "<unknown>",
							result->principal_name != NULL ? result->principal_name : "<unknown>",
							result->principal_runtime != NULL ? result->principal_runtime : "<unknown>",
							result->provider_name != NULL ? result->provider_name : "<unknown>",
							result->provider_kind != NULL ? result->provider_kind : "<unknown>",
							result->sandbox_name != NULL ? result->sandbox_name : "<unknown>",
							result->profile_name != NULL ? result->profile_name : "<unknown>",
							result->environment_mode != NULL ? result->environment_mode : "<unknown>",
							result->workdir_name != NULL ? result->workdir_name : "<unknown>",
							result->process_limit,
							result->timeout_ms,
							result->receipt_schema != NULL ? result->receipt_schema : "<unknown>",
							result->receipt_alg != NULL ? result->receipt_alg : "<unknown>",
							result->receipt_nonce != NULL ? result->receipt_nonce : "<unknown>",
							result->receipt_signature != NULL ? "verified" : "missing",
							result->attestation_mode != NULL ? result->attestation_mode : "<unknown>",
							result->container_id != NULL ? result->container_id : "",
							result->vm_id != NULL ? result->vm_id : "",
							result->token_charge,
							result->cost_charge,
							detail_for_payload != NULL ? detail_for_payload : "<none>");
		if (detail_for_payload != NULL)
			pfree(detail_for_payload);
	}
	stripped = qx_strip_external_trace_payload(payload);
	pfree(payload);

	return stripped;
}

static void
qx_validate_external_result(const QxExternalToolResult *result,
							const QxSandboxProfile *profile,
							const char *phase,
							Oid taskoid,
							const char *expected_tool,
							const char *expected_principal,
							const char *expected_principal_runtime,
							const char *expected_provider,
							const char *expected_provider_kind,
							const char *expected_provider_endpoint,
							const char *expected_receipt_schema,
							const char *expected_receipt_alg,
							const char *expected_receipt_nonce,
							const char *receipt_key,
							bool require_attestation)
{
	char	   *expected_attestation;
	char	   *receipt_payload;

	if (expected_tool != NULL &&
		(result->tool_name == NULL ||
		 strcmp(result->tool_name, expected_tool) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned tool mismatch"),
				 errdetail("Expected tool \"%s\" but got \"%s\".",
						   expected_tool,
						   result->tool_name != NULL ? result->tool_name : "<null>")));

	if (expected_principal != NULL &&
		(result->principal_name == NULL ||
		 strcmp(result->principal_name, expected_principal) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned principal mismatch"),
				 errdetail("Expected principal \"%s\" but got \"%s\".",
						   expected_principal,
						   result->principal_name != NULL ? result->principal_name : "<null>")));

	if (expected_principal_runtime != NULL &&
		(result->principal_runtime == NULL ||
		 strcmp(result->principal_runtime, expected_principal_runtime) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned principal-runtime mismatch"),
				 errdetail("Expected principal runtime \"%s\" but got \"%s\".",
						   expected_principal_runtime,
						   result->principal_runtime != NULL ? result->principal_runtime : "<null>")));

	if (expected_provider != NULL &&
		(result->provider_name == NULL ||
		 strcmp(result->provider_name, expected_provider) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned provider mismatch"),
				 errdetail("Expected provider \"%s\" but got \"%s\".",
						   expected_provider,
						   result->provider_name != NULL ? result->provider_name : "<null>")));

	if (expected_provider_kind != NULL &&
		(result->provider_kind == NULL ||
		 strcmp(result->provider_kind, expected_provider_kind) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned provider-kind mismatch"),
				 errdetail("Expected provider kind \"%s\" but got \"%s\".",
						   expected_provider_kind,
						   result->provider_kind != NULL ? result->provider_kind : "<null>")));

	if (result->profile_name == NULL ||
		strcmp(result->profile_name, profile->name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned sandbox profile mismatch"),
				 errdetail("Expected profile \"%s\" but got \"%s\".",
						   profile->name,
						   result->profile_name != NULL ? result->profile_name : "<null>")));

	if (result->environment_mode == NULL ||
		strcmp(result->environment_mode, "minimal") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response did not run under a minimal sandbox environment")));

	if (result->workdir_name == NULL ||
		strcmp(result->workdir_name, "pg_qx_runtime") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response escaped the runtime workdir"),
				 errdetail("Expected workdir basename \"pg_qx_runtime\" but got \"%s\".",
						   result->workdir_name != NULL ? result->workdir_name : "<null>")));

	if (result->timeout_ms != profile->timeout_ms)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response reported timeout mismatch"),
				 errdetail("Expected %d ms but got %d ms.",
						   profile->timeout_ms, result->timeout_ms)));

	if (result->process_limit != profile->process_limit)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response reported process-limit mismatch"),
				 errdetail("Expected %d but got %d.",
						   profile->process_limit, result->process_limit)));

	if (result->path_present)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response observed PATH in sandbox environment"),
				 errdetail("Stage 17 requires PATH to be stripped from external principal execution.")));

	if (expected_receipt_schema != NULL &&
		(result->receipt_schema == NULL ||
		 strcmp(result->receipt_schema, expected_receipt_schema) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned receipt schema mismatch"),
				 errdetail("Expected receipt schema \"%s\" but got \"%s\".",
						   expected_receipt_schema,
						   result->receipt_schema != NULL ? result->receipt_schema : "<null>")));

	if (expected_receipt_alg != NULL &&
		(result->receipt_alg == NULL ||
		 strcmp(result->receipt_alg, expected_receipt_alg) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned receipt algorithm mismatch"),
				 errdetail("Expected receipt algorithm \"%s\" but got \"%s\".",
						   expected_receipt_alg,
						   result->receipt_alg != NULL ? result->receipt_alg : "<null>")));

	if (expected_receipt_nonce != NULL &&
		(result->receipt_nonce == NULL ||
		 strcmp(result->receipt_nonce, expected_receipt_nonce) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned receipt nonce mismatch"),
				 errdetail("Expected receipt nonce \"%s\" but got \"%s\".",
						   expected_receipt_nonce,
						   result->receipt_nonce != NULL ? result->receipt_nonce : "<null>")));

	expected_attestation = qx_expected_attestation_mode(expected_provider_kind,
														 require_attestation);
	if (result->attestation_mode == NULL ||
		strcmp(result->attestation_mode, expected_attestation) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response failed attestation requirements"),
				 errdetail("Expected attestation mode \"%s\" but got \"%s\".",
						   expected_attestation,
						   result->attestation_mode != NULL ? result->attestation_mode : "<null>")));

	if (result->receipt_signature == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response omitted a signed receipt"),
				 errdetail("Providers must return RECEIPT_SIG so the runtime can verify execution evidence.")));

	receipt_payload = qx_receipt_payload(phase,
										 taskoid,
										 expected_tool,
										 expected_principal,
										 expected_principal_runtime,
										 expected_provider,
										 expected_provider_kind,
										 expected_provider_endpoint,
										 result->sandbox_name,
										 result->profile_name,
										 result->environment_mode,
										 result->workdir_name,
										 result->timeout_ms,
										 result->process_limit,
										 result->path_present,
										 result->receipt_schema,
										 result->receipt_alg,
										 result->receipt_nonce,
										 result->attestation_mode,
										 result->token_charge,
										 result->cost_charge,
										 result->detail);

	if (expected_receipt_alg != NULL &&
		strcmp(expected_receipt_alg, "ed25519") == 0)
	{
#ifdef USE_OPENSSL
		if (!qx_verify_ed25519_receipt_signature(receipt_key,
												 receipt_payload,
												 result->receipt_signature))
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("principal response returned an invalid ed25519 receipt signature")));
#else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ed25519 receipt verification requires OpenSSL support")));
#endif
	}
	else
	{
		char	   *expected_signature;

		expected_signature = qx_receipt_hmac_signature_hex(receipt_key,
														   receipt_payload);
		if (strcmp(result->receipt_signature, expected_signature) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("principal response returned an invalid receipt signature"),
					 errdetail("Expected HMAC-SHA256 signature \"%s\" but got \"%s\".",
							   expected_signature,
							   result->receipt_signature)));
		pfree(expected_signature);
	}
	pfree(expected_attestation);
	pfree(receipt_payload);
}

static void
qx_free_external_result(QxExternalToolResult *result)
{
	if (result->detail != NULL)
		pfree(result->detail);
	if (result->tool_name != NULL)
		pfree(result->tool_name);
	if (result->principal_name != NULL)
		pfree(result->principal_name);
	if (result->principal_runtime != NULL)
		pfree(result->principal_runtime);
	if (result->provider_name != NULL)
		pfree(result->provider_name);
	if (result->provider_kind != NULL)
		pfree(result->provider_kind);
	if (result->provider_endpoint != NULL)
		pfree(result->provider_endpoint);
	if (result->sandbox_name != NULL)
		pfree(result->sandbox_name);
	if (result->profile_name != NULL)
		pfree(result->profile_name);
	if (result->environment_mode != NULL)
		pfree(result->environment_mode);
	if (result->workdir_name != NULL)
		pfree(result->workdir_name);
	if (result->launch_mode != NULL)
		pfree(result->launch_mode);
	if (result->receipt_schema != NULL)
		pfree(result->receipt_schema);
	if (result->receipt_alg != NULL)
		pfree(result->receipt_alg);
	if (result->receipt_nonce != NULL)
		pfree(result->receipt_nonce);
	if (result->receipt_signature != NULL)
		pfree(result->receipt_signature);
	if (result->attestation_mode != NULL)
		pfree(result->attestation_mode);
	memset(result, 0, sizeof(*result));
}

static void
qx_build_semantic_execution_metadata(QxSemanticExecutionMetadata *metadata,
									 const QxExternalToolResult *result)
{
	memset(metadata, 0, sizeof(*metadata));

	metadata->provider_name = result->provider_name;
	metadata->provider_kind = result->provider_kind;
	metadata->provider_endpoint = result->provider_endpoint;
	metadata->principal_name = result->principal_name;
	metadata->principal_runtime = result->principal_runtime;
	metadata->sandbox_name = result->sandbox_name;
	metadata->profile_name = result->profile_name;
	metadata->environment_mode = result->environment_mode;
	metadata->workdir_name = result->workdir_name;
	metadata->launch_mode = result->launch_mode;
	metadata->receipt_schema = result->receipt_schema;
	metadata->receipt_alg = result->receipt_alg;
	metadata->receipt_nonce = result->receipt_nonce;
	metadata->attestation_mode = result->attestation_mode;
	metadata->timeout_ms = result->timeout_ms;
	metadata->process_limit = result->process_limit;
	metadata->wall_time_ms = result->wall_time_ms;
	metadata->token_charge = result->token_charge;
	metadata->cost_charge = result->cost_charge;
	metadata->path_present = result->path_present;
	metadata->restricted_identity = result->restricted_identity;
}

static void
qx_execute_tool_contract(const char *contract, const char *phase, Oid taskoid,
						 const char *goal, bool input_present,
						 QxExternalToolResult *result)
{
	char	   *tool_name;
	char	   *handler_name;
	char	   *tool_sandbox;
	char	   *principal_name;
	char	   *principal_sandbox;
	char	   *principal_runtime;
	char	   *program_name;
	char	   *receipt_signer;
	char	   *provider_name;
	char	   *provider_oid_str;
	char	   *provider_kind;
	char	   *provider_endpoint;
	char	   *provider_attestation;
	char	   *receipt_schema;
	char	   *receipt_alg;
	char	   *receipt_nonce;
	char	   *receipt_key;
	char	   *expected_attestation;
	const char *effective_sandbox;
	const QxSandboxProfile *profile;
	const QxSandboxProfile *execution_profile;
	QxSandboxProfile microvm_profile;
	QxSandboxObservation observation;
	char		runtime_dir[MAXPGPATH];
	char		program_path[MAXPGPATH];
	char		signer_path[MAXPGPATH];
	char		docker_cli_path[MAXPGPATH];
	char		microvm_qemu_path[MAXPGPATH];
	char		microvm_kernel_path[MAXPGPATH];
	char		microvm_initrd_path[MAXPGPATH];
	char		request_path[MAXPGPATH];
	char		response_path[MAXPGPATH];
	char		launch_request_path[MAXPGPATH];
	char	   *request_payload;
	char	   *launch_request_payload = NULL;
	char	   *tool_capability_tags;
	char	   *container_request_lines = NULL;
	char	   *microvm_request_lines = NULL;
	QxRuntimePolicy runtime_policy;
	Oid			provideroid = InvalidOid;
	const char *resolved_signer = "";
	bool		use_container_backend = false;
	bool		use_microvm_backend = false;

	if (contract == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no authorized tool contract to execute",
						taskoid)));

	tool_name = qx_contract_value(contract, "tool");
	handler_name = qx_contract_value(contract, "handler");
	tool_sandbox = qx_contract_value(contract, "tool_sandbox");
	principal_name = qx_contract_value(contract, "principal");
	principal_sandbox = qx_contract_value(contract, "principal_sandbox");
	principal_runtime = qx_contract_value(contract, "principal_runtime");
	program_name = qx_contract_value(contract, "program");
	receipt_signer = qx_contract_value(contract, "receipt_signer");
	provider_name = qx_contract_value(contract, "provider");
	provider_oid_str = qx_contract_value(contract, "provider_oid");
	provider_kind = qx_contract_value(contract, "provider_kind");
	provider_endpoint = qx_contract_value(contract, "provider_endpoint");
	provider_attestation = qx_contract_value(contract, "provider_attestation");
	receipt_schema = qx_contract_value(contract, "receipt_schema");
	receipt_alg = qx_contract_value(contract, "receipt_alg");
	tool_capability_tags = qx_contract_value(contract, "tool_capability_tags");
	receipt_nonce = psprintf("%s:%s:%s",
							 phase != NULL ? phase : "submit",
							 tool_name != NULL ? tool_name : "tool",
							 principal_name != NULL ? principal_name : "principal");
	qx_runtime_policy_init(&runtime_policy);
	qx_runtime_policy_from_authz(NULL, tool_capability_tags, &runtime_policy);
	if (principal_runtime == NULL || principal_runtime[0] == '\0')
	{
		if (principal_runtime != NULL)
			pfree(principal_runtime);
		principal_runtime =
			pstrdup(qx_default_runtime_for_provider_kind(provider_kind));
	}
	expected_attestation = qx_expected_attestation_mode(provider_kind,
														 provider_attestation != NULL &&
														 strcmp(provider_attestation, "required") == 0);

	if (provider_oid_str == NULL || provider_oid_str[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tool contract is missing provider_oid for receipt verification")));
	provideroid = (Oid) strtoul(provider_oid_str, NULL, 10);
	if (!OidIsValid(provideroid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tool contract carried an invalid provider_oid \"%s\"",
						provider_oid_str)));
	receipt_key = qx_provider_receipt_key(provideroid);
	if (receipt_alg != NULL && strcmp(receipt_alg, "ed25519") == 0)
	{
		if (receipt_signer == NULL || receipt_signer[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tool contract is missing receipt_signer for ed25519 verification")));
		qx_resolve_receipt_signer_path(receipt_signer,
									   signer_path,
									   sizeof(signer_path));
		resolved_signer = signer_path;
	}

	if (qx_sandbox_rank(tool_sandbox) > qx_sandbox_rank(principal_sandbox))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("tool \"%s\" exceeds principal \"%s\" sandbox ceiling",
						tool_name != NULL ? tool_name : "<unknown>",
						principal_name != NULL ? principal_name : "<unknown>")));

	effective_sandbox = qx_effective_sandbox_name(tool_sandbox,
												  principal_sandbox);
	profile = qx_lookup_sandbox_profile(effective_sandbox);
	execution_profile = profile;
	use_container_backend =
		qx_container_backend_is_supported_provider_kind(provider_kind) &&
		qx_container_backend_is_supported_runtime_class(principal_runtime);
	use_microvm_backend =
		qx_microvm_backend_is_supported_provider_kind(provider_kind) &&
		qx_microvm_backend_is_supported_runtime_class(principal_runtime) &&
		qx_resolve_microvm_runtime_paths(microvm_kernel_path,
											 sizeof(microvm_kernel_path),
											 microvm_initrd_path,
											 sizeof(microvm_initrd_path));
	if (use_microvm_backend)
	{
		microvm_profile = *profile;
		if (microvm_profile.timeout_ms < QX_MICROVM_TIMEOUT_FLOOR_MS)
			microvm_profile.timeout_ms = QX_MICROVM_TIMEOUT_FLOOR_MS;
		if (microvm_profile.process_limit < 4)
			microvm_profile.process_limit = 4;
		if (microvm_profile.memory_kb < 262144)
			microvm_profile.memory_kb = 262144;
		execution_profile = &microvm_profile;
	}
	if (use_container_backend)
		qx_runtime_policy_compile_container(&runtime_policy);
	if (use_microvm_backend)
		qx_runtime_policy_resolve_microvm_assets(&runtime_policy,
												 microvm_kernel_path,
												 microvm_initrd_path);
	qx_validate_container_runtime_contract(phase,
										   taskoid,
										   tool_name,
										   handler_name,
										   effective_sandbox,
										   execution_profile,
										   principal_name,
										   principal_runtime,
										   provider_name,
										   provider_kind,
										   provider_endpoint,
										   receipt_schema != NULL ? receipt_schema : "qx.receipt.v1",
										   receipt_alg != NULL ? receipt_alg : "hmac-sha256",
										   receipt_nonce,
										   provider_attestation != NULL &&
										   strcmp(provider_attestation, "required") == 0);
	qx_validate_microvm_runtime_contract(phase,
										 taskoid,
										 tool_name,
										 handler_name,
										 effective_sandbox,
										 execution_profile,
										 principal_name,
										 principal_runtime,
										 provider_name,
										 provider_kind,
										 provider_endpoint,
										 receipt_schema != NULL ? receipt_schema : "qx.receipt.v1",
										 receipt_alg != NULL ? receipt_alg : "hmac-sha256",
										 receipt_nonce,
										 provider_attestation != NULL &&
										 strcmp(provider_attestation, "required") == 0);
	if (use_container_backend)
	{
		const char *container_image = getenv("QX_CONTAINER_IMAGE");

		qx_resolve_docker_cli_path(docker_cli_path, sizeof(docker_cli_path));
		container_request_lines = psprintf(
			"DOCKER_CLI=%s\nDOCKER_HOST=%s\nCONTAINER_IMAGE=%s\n",
			docker_cli_path,
			qx_default_docker_host(),
			(container_image != NULL && container_image[0] != '\0') ?
			container_image : "alpine:3.20");
	}
	if (use_microvm_backend)
	{
		const char *microvm_accel = getenv("QX_MICROVM_ACCEL");

		qx_resolve_microvm_qemu_path(microvm_qemu_path, sizeof(microvm_qemu_path));
		microvm_request_lines = psprintf(
			"QEMU_CLI=%s\nMICROVM_KERNEL=%s\nMICROVM_INITRD=%s\nMICROVM_ACCEL=%s\n",
			microvm_qemu_path,
			microvm_kernel_path,
			microvm_initrd_path,
			(microvm_accel != NULL && microvm_accel[0] != '\0') ?
			microvm_accel : "tcg");
	}
	qx_resolve_principal_program_path(program_name, program_path, sizeof(program_path));
	qx_runtime_temp_dir(runtime_dir, sizeof(runtime_dir));
	qx_runtime_temp_path(request_path, sizeof(request_path), "req");
	qx_runtime_temp_path(response_path, sizeof(response_path), "resp");
	launch_request_path[0] = '\0';
	if (use_container_backend)
	{
		QxContainerBackendRequest backend_request;

		qx_runtime_temp_path(launch_request_path, sizeof(launch_request_path),
							 "launch");
		qx_container_backend_request_init(&backend_request);
		backend_request.phase = pstrdup(phase != NULL ? phase : "submit");
		backend_request.tool_name = pstrdup(qx_safe_runtime_text(tool_name));
		backend_request.principal_name = pstrdup(qx_safe_runtime_text(principal_name));
		backend_request.principal_runtime = pstrdup(qx_safe_runtime_text(principal_runtime));
		backend_request.provider_name = pstrdup(qx_safe_runtime_text(provider_name));
		backend_request.provider_kind = pstrdup(qx_safe_runtime_text(provider_kind));
		backend_request.provider_endpoint = pstrdup(qx_safe_runtime_text(provider_endpoint));
		backend_request.sandbox_name = pstrdup(qx_safe_runtime_text(effective_sandbox));
		backend_request.profile_name = pstrdup(execution_profile->name);
		backend_request.environment_mode = pstrdup("minimal");
		backend_request.workdir_name = pstrdup("pg_qx_runtime");
		backend_request.command_line = psprintf("docker run --rm %s true",
												runtime_policy.image_ref);
		qx_runtime_policy_validate_image_ref(runtime_policy.image_ref);
		backend_request.image_ref = pstrdup(runtime_policy.image_ref);
		backend_request.receipt_schema = pstrdup(receipt_schema != NULL ? receipt_schema : "qx.receipt.v1");
		backend_request.receipt_alg = pstrdup(receipt_alg != NULL ? receipt_alg : "hmac-sha256");
		backend_request.receipt_nonce = pstrdup(receipt_nonce);
		backend_request.attestation_mode =
			pstrdup(qx_container_backend_expected_attestation_mode());
		backend_request.detail = psprintf("task=%u", taskoid);
		backend_request.timeout_ms = execution_profile->timeout_ms;
		backend_request.memory_kb = execution_profile->memory_kb;
		backend_request.process_limit = execution_profile->process_limit;
		backend_request.require_attestation =
			(provider_attestation != NULL &&
			 strcmp(provider_attestation, "required") == 0);
		backend_request.allow_network = runtime_policy.allow_network;
		backend_request.allow_privilege_escalation =
			runtime_policy.allow_privilege_escalation;
		backend_request.readonly_rootfs = runtime_policy.readonly_rootfs;
		backend_request.seccomp_mode =
			pstrdup(runtime_policy.seccomp_mode != NULL ?
					runtime_policy.seccomp_mode : "no-new-privileges");
		backend_request.oci_profile =
			pstrdup(runtime_policy.oci_profile != NULL ?
					runtime_policy.oci_profile : "");
		launch_request_payload =
			qx_container_backend_build_launch_request(&backend_request);
		qx_container_backend_request_free(&backend_request);
	}
	else if (use_microvm_backend)
	{
		QxMicrovmBackendRequest backend_request;

		qx_runtime_temp_path(launch_request_path, sizeof(launch_request_path),
							 "launch");
		qx_microvm_backend_request_init(&backend_request);
		backend_request.phase = pstrdup(phase != NULL ? phase : "submit");
		backend_request.tool_name = pstrdup(qx_safe_runtime_text(tool_name));
		backend_request.principal_name = pstrdup(qx_safe_runtime_text(principal_name));
		backend_request.principal_runtime = pstrdup(qx_safe_runtime_text(principal_runtime));
		backend_request.provider_name = pstrdup(qx_safe_runtime_text(provider_name));
		backend_request.provider_kind = pstrdup(qx_safe_runtime_text(provider_kind));
		backend_request.provider_endpoint = pstrdup(qx_safe_runtime_text(provider_endpoint));
		backend_request.sandbox_name = pstrdup(qx_safe_runtime_text(effective_sandbox));
		backend_request.profile_name = pstrdup(execution_profile->name);
		backend_request.environment_mode = pstrdup("minimal");
		backend_request.workdir_name = pstrdup("pg_qx_runtime");
		backend_request.command_line = psprintf("%s -M microvm -kernel %s -initrd %s",
											  microvm_qemu_path,
											  runtime_policy.kernel_ref,
											  runtime_policy.initrd_ref);
		backend_request.kernel_ref = pstrdup(runtime_policy.kernel_ref);
		backend_request.initrd_ref = pstrdup(runtime_policy.initrd_ref);
		backend_request.image_ref = psprintf("kernel=%s;initrd=%s",
											 runtime_policy.kernel_ref,
											 runtime_policy.initrd_ref);
		backend_request.snapshot_ref =
			runtime_policy.snapshot_ref != NULL ?
			pstrdup(runtime_policy.snapshot_ref) : NULL;
		backend_request.receipt_schema = pstrdup(receipt_schema != NULL ? receipt_schema : "qx.receipt.v1");
		backend_request.receipt_alg = pstrdup(receipt_alg != NULL ? receipt_alg : "hmac-sha256");
		backend_request.receipt_nonce = pstrdup(receipt_nonce);
		backend_request.attestation_mode =
			pstrdup(qx_microvm_backend_expected_attestation_mode());
		backend_request.detail = psprintf("task=%u", taskoid);
		backend_request.timeout_ms = execution_profile->timeout_ms;
		backend_request.memory_kb = execution_profile->memory_kb;
		backend_request.process_limit = execution_profile->process_limit;
		backend_request.vcpu_count = 1;
		backend_request.require_attestation =
			(provider_attestation != NULL &&
			 strcmp(provider_attestation, "required") == 0);
		backend_request.allow_network = runtime_policy.allow_network;
		backend_request.allow_privilege_escalation =
			runtime_policy.allow_privilege_escalation;
		launch_request_payload =
			qx_microvm_backend_build_launch_request(&backend_request);
		qx_microvm_backend_request_free(&backend_request);
	}
	if (launch_request_payload != NULL)
		qx_write_text_file(launch_request_path, launch_request_payload);

	request_payload = psprintf(
		"PHASE=%s\nTASK_OID=%u\nGOAL_LENGTH=%zu\nINPUT_PRESENT=%s\nTOOL=%s\nHANDLER=%s\nSANDBOX=%s\nPRINCIPAL=%s\nPRINCIPAL_RUNTIME=%s\nPROVIDER=%s\nPROVIDER_KIND=%s\nPROVIDER_ENDPOINT=%s\nREQUIRE_ATTESTATION=%s\nRECEIPT_SCHEMA=%s\nRECEIPT_ALG=%s\nRECEIPT_NONCE=%s\nRECEIPT_KEY=%s\nRECEIPT_SIGNER=%s\n",
		phase,
		taskoid,
		goal != NULL ? strlen(goal) : 0,
		input_present ? "true" : "false",
		tool_name != NULL ? tool_name : "",
		handler_name != NULL ? handler_name : "",
		tool_sandbox != NULL ? tool_sandbox : "builtin",
		principal_name != NULL ? principal_name : "",
		principal_runtime != NULL ? principal_runtime : "host",
		provider_name != NULL ? provider_name : "",
		provider_kind != NULL ? provider_kind : "loopback",
		provider_endpoint != NULL ? provider_endpoint : "local://qhapaqxian-tool-runner",
		(provider_attestation != NULL && strcmp(provider_attestation, "required") == 0) ? "true" : "false",
		receipt_schema != NULL ? receipt_schema : "qx.receipt.v1",
		receipt_alg != NULL ? receipt_alg : "hmac-sha256",
		receipt_nonce,
		(receipt_alg != NULL && strcmp(receipt_alg, "ed25519") == 0) ? "" : receipt_key,
		resolved_signer);
	if (container_request_lines != NULL)
	{
		char	   *augmented_payload;

		augmented_payload = psprintf("%s%s",
									 request_payload,
									 container_request_lines);
		pfree(request_payload);
		request_payload = augmented_payload;
	}
	if (microvm_request_lines != NULL)
	{
		char	   *augmented_payload;

		augmented_payload = psprintf("%s%s",
									 request_payload,
									 microvm_request_lines);
		pfree(request_payload);
		request_payload = augmented_payload;
	}
	if (launch_request_path[0] != '\0')
	{
		char	   *augmented_payload;

		augmented_payload = psprintf("%sLAUNCH_REQUEST_FILE=%s\n",
									 request_payload,
									 launch_request_path);
		pfree(request_payload);
		request_payload = augmented_payload;
	}
	qx_write_text_file(request_path, request_payload);
	pfree(request_payload);
	if (launch_request_payload != NULL)
		pfree(launch_request_payload);
	if (container_request_lines != NULL)
		pfree(container_request_lines);
	if (microvm_request_lines != NULL)
		pfree(microvm_request_lines);

	memset(&observation, 0, sizeof(observation));
	qx_launch_principal_program(program_path, execution_profile, runtime_dir,
								request_path, response_path,
								launch_request_path[0] != '\0' ?
								launch_request_path : NULL,
								(use_container_backend || use_microvm_backend),
								&observation);
	qx_read_external_result(response_path, result);
	if (use_container_backend && result->detail != NULL &&
		strstr(result->detail, "backend_launch=docker") != NULL)
	{
		const char *instance_id = result->container_id;

		if (instance_id == NULL || instance_id[0] == '\0')
		{
			char		fallback_id[64];

			snprintf(fallback_id, sizeof(fallback_id), "qx-container-%u", taskoid);
			instance_id = fallback_id;
		}
		qx_backend_supervisor_register(taskoid, instance_id,
									   QX_BACKEND_LEASE_CONTAINER);
	}
	else if (use_microvm_backend && result->detail != NULL &&
			 strstr(result->detail, "backend_launch=qemu") != NULL)
	{
		const char *instance_id = result->vm_id;

		if (instance_id == NULL || instance_id[0] == '\0')
		{
			char		fallback_id[64];

			snprintf(fallback_id, sizeof(fallback_id), "qx-microvm-%u", taskoid);
			instance_id = fallback_id;
		}
		qx_backend_supervisor_register(taskoid, instance_id,
									   QX_BACKEND_LEASE_MICROVM);
	}
	qx_validate_external_result(result, execution_profile,
								phase,
								taskoid,
								tool_name,
								principal_name,
								principal_runtime != NULL ? principal_runtime : "host",
								provider_name,
								provider_kind != NULL ? provider_kind : "loopback",
								provider_endpoint != NULL ? provider_endpoint : "local://qhapaqxian-tool-runner",
								receipt_schema != NULL ? receipt_schema : "qx.receipt.v1",
								receipt_alg != NULL ? receipt_alg : "hmac-sha256",
								receipt_nonce,
								receipt_key,
								provider_attestation != NULL &&
								strcmp(provider_attestation, "required") == 0);

	if (unlink(request_path) != 0 && errno != ENOENT)
		elog(WARNING, "could not remove QhapaqXian request file \"%s\": %m",
			 request_path);
	if (unlink(response_path) != 0 && errno != ENOENT)
		elog(WARNING, "could not remove QhapaqXian response file \"%s\": %m",
			 response_path);
	if (launch_request_path[0] != '\0' &&
		unlink(launch_request_path) != 0 && errno != ENOENT)
		elog(WARNING, "could not remove QhapaqXian launch request file \"%s\": %m",
			 launch_request_path);

	if (result->tool_name == NULL && tool_name != NULL)
		result->tool_name = pstrdup(tool_name);
	if (result->principal_name == NULL && principal_name != NULL)
		result->principal_name = pstrdup(principal_name);
	if (result->principal_runtime == NULL)
		result->principal_runtime = pstrdup(principal_runtime != NULL ? principal_runtime : "host");
	if (result->provider_name == NULL && provider_name != NULL)
		result->provider_name = pstrdup(provider_name);
	if (result->provider_kind == NULL)
		result->provider_kind = pstrdup(provider_kind != NULL ? provider_kind : "loopback");
	if (result->provider_endpoint == NULL && provider_endpoint != NULL)
		result->provider_endpoint = pstrdup(provider_endpoint);
	if (result->sandbox_name == NULL)
		result->sandbox_name = pstrdup(effective_sandbox);
	if (result->profile_name == NULL)
		result->profile_name = pstrdup(profile->name);
	if (result->environment_mode == NULL)
		result->environment_mode = pstrdup("minimal");
	if (result->workdir_name == NULL)
		result->workdir_name = pstrdup("pg_qx_runtime");
	if (result->launch_mode == NULL)
		result->launch_mode = pstrdup(observation.launch_mode != NULL ? observation.launch_mode : "profiled_process");
	if (result->receipt_schema == NULL)
		result->receipt_schema = pstrdup(receipt_schema != NULL ? receipt_schema : "qx.receipt.v1");
	if (result->receipt_alg == NULL)
		result->receipt_alg = pstrdup(receipt_alg != NULL ? receipt_alg : "hmac-sha256");
	if (result->receipt_nonce == NULL)
		result->receipt_nonce = pstrdup(receipt_nonce);
	if (result->attestation_mode == NULL)
		result->attestation_mode = expected_attestation;
	else
		pfree(expected_attestation);
	if (result->receipt_signature == NULL)
		result->receipt_signature = pstrdup("verified");
	if (result->timeout_ms == 0)
		result->timeout_ms = profile->timeout_ms;
	if (result->process_limit == 0)
		result->process_limit = profile->process_limit;
	result->restricted_identity = observation.restricted_identity;
	result->wall_time_ms = observation.wall_time_ms;
	if (result->detail != NULL)
	{
		char	   *augmented_detail;
		char	   *clean_detail;
		const char *launch_tail;

		clean_detail = qx_strip_duplicate_receipt_tail(result->detail);
		{
			const char *tokens_in_detail = strstr(clean_detail, ";tokens=");

			if (tokens_in_detail != NULL)
			{
				char	   *truncated_detail;

				truncated_detail = pnstrdup(clean_detail,
											tokens_in_detail - clean_detail);
				pfree(clean_detail);
				clean_detail = truncated_detail;
			}
		}
		pfree(result->detail);
		result->detail = clean_detail;
		launch_tail = strstr(result->detail, ";launch_mode=");

		if (launch_tail != NULL)
		{
			char	   *detail_base;

			detail_base = pnstrdup(result->detail,
								   launch_tail - result->detail);
			pfree(result->detail);
			result->detail = detail_base;
		}

		augmented_detail = psprintf("%s;launch_mode=%s;restricted_identity=%s;wall_ms=%d",
									result->detail,
									observation.launch_mode != NULL ? observation.launch_mode : "profiled_process",
									observation.restricted_identity ? "true" : "false",
									observation.wall_time_ms);
		pfree(result->detail);
		result->detail = qx_truncate_detail_after_wall_ms(augmented_detail);
		pfree(augmented_detail);
	}

	if (result->container_id != NULL && result->container_id[0] != '\0')
		qx_backend_supervisor_release(taskoid, result->container_id);
	else if (result->vm_id != NULL && result->vm_id[0] != '\0')
		qx_backend_supervisor_release(taskoid, result->vm_id);
	else if (use_container_backend || use_microvm_backend)
		qx_backend_supervisor_fence_stale(taskoid);

	if (receipt_signer != NULL)
		pfree(receipt_signer);

	if (tool_name != NULL)
		pfree(tool_name);
	if (handler_name != NULL)
		pfree(handler_name);
	if (tool_sandbox != NULL)
		pfree(tool_sandbox);
	if (principal_name != NULL)
		pfree(principal_name);
	if (principal_sandbox != NULL)
		pfree(principal_sandbox);
	if (principal_runtime != NULL)
		pfree(principal_runtime);
	if (program_name != NULL)
		pfree(program_name);
	if (provider_name != NULL)
		pfree(provider_name);
	if (provider_oid_str != NULL)
		pfree(provider_oid_str);
	if (provider_kind != NULL)
		pfree(provider_kind);
	if (provider_endpoint != NULL)
		pfree(provider_endpoint);
	if (provider_attestation != NULL)
		pfree(provider_attestation);
	if (receipt_schema != NULL)
		pfree(receipt_schema);
	if (receipt_alg != NULL)
		pfree(receipt_alg);
	if (receipt_nonce != NULL)
		pfree(receipt_nonce);
	if (receipt_key != NULL)
		pfree(receipt_key);
	if (tool_capability_tags != NULL)
		pfree(tool_capability_tags);
	qx_runtime_policy_free(&runtime_policy);
}

static List *
qx_parse_task_authorized_contracts(const char *serialized)
{
	Node	   *node;

	if (serialized == NULL)
		return NIL;

	node = stringToNode(serialized);
	if (node == NULL)
		return NIL;
	if (!IsA(node, List))
		elog(ERROR, "QhapaqXian authorized tool payload was not a List");

	return castNode(List, node);
}

static const char *
qx_runtime_request_phase_contract(const QxRuntimeTaskRequest *request,
								  bool prefer_resume)
{
	const char *selected_contract;

	if (request == NULL)
		return NULL;

	selected_contract = prefer_resume ?
		request->resume_contract :
		request->submit_contract;
	if (selected_contract != NULL && selected_contract[0] != '\0')
		return selected_contract;

	if (request->authorized_tools == NIL)
		return NULL;

	return strVal((Node *) (prefer_resume ?
							llast(request->authorized_tools) :
							linitial(request->authorized_tools)));
}

static char *
qx_runtime_task_phase_contract(const QxCatalogTaskInfo *task, bool prefer_resume)
{
	List	   *authorized_contracts;
	Node	   *selected_node;
	const char *selected_contract;
	char	   *copied_contract = NULL;

	if (task == NULL)
		return NULL;

	selected_contract = prefer_resume ?
		task->resume_contract :
		task->submit_contract;
	if (selected_contract != NULL && selected_contract[0] != '\0')
		return pstrdup(selected_contract);

	authorized_contracts = qx_parse_task_authorized_contracts(task->authorized_tools);
	if (authorized_contracts != NIL)
	{
		selected_node = prefer_resume ?
			(Node *) llast(authorized_contracts) :
			(Node *) linitial(authorized_contracts);
		copied_contract = pstrdup(strVal(selected_node));
	}

	if (authorized_contracts != NIL)
		list_free_deep(authorized_contracts);

	return copied_contract;
}


static int32
qx_runtime_lease_ttl_ms(const QxSchedulerLeaseSnapshot *snapshot)
{
	TimestampTz	ttl_usecs;

	if (snapshot == NULL)
		return 1;

	ttl_usecs = snapshot->expires_at - snapshot->renewed_at;
	if (ttl_usecs <= 0)
		return 1;

	return Max((int32) (ttl_usecs / USECS_PER_MSEC), 1);
}

static void
qx_runtime_free_scheduler_queue_snapshot(QxSchedulerQueueSnapshot *snapshot)
{
	QxCatalogFreeSchedulerQueueSnapshot(snapshot);
}

static int32
qx_runtime_scheduler_max_retries(void)
{
	return QX_SCHEDULER_MAX_RETRIES;
}

static int32
qx_runtime_scheduler_slot_count(void)
{
	const char *slot_env;
	char	   *endptr;
	long		slot_count;

	slot_env = getenv("QX_SCHEDULER_DB_WORKER_SLOTS");
	if (slot_env == NULL || slot_env[0] == '\0')
		return QX_SCHEDULER_DB_WORKER_SLOTS_DEFAULT;

	errno = 0;
	slot_count = strtol(slot_env, &endptr, 10);
	if (errno != 0 || endptr == slot_env || *endptr != '\0')
		return QX_SCHEDULER_DB_WORKER_SLOTS_DEFAULT;

	if (slot_count < QX_SCHEDULER_DB_WORKER_SLOTS_MIN)
		return QX_SCHEDULER_DB_WORKER_SLOTS_MIN;
	if (slot_count > QX_SCHEDULER_DB_WORKER_SLOTS_MAX)
		return QX_SCHEDULER_DB_WORKER_SLOTS_MAX;

	return (int32) slot_count;
}

static int32
qx_runtime_scheduler_owner_slot(Oid taskoid, int32 slot_count)
{
	uint32		hash;

	if (slot_count <= 1)
		return 0;

	/* A small reversible mix avoids parity-locking on monotonic OIDs. */
	hash = (uint32) taskoid;
	hash ^= hash >> 16;
	hash *= UINT32_C(0x7feb352d);
	hash ^= hash >> 15;
	hash *= UINT32_C(0x846ca68b);
	hash ^= hash >> 16;

	return (int32) (hash % (uint32) slot_count);
}

static bool
qx_runtime_scheduler_worker_owns_task(Oid taskoid,
									  int32 slot_index,
									  int32 slot_count)
{
	if (slot_count <= 1)
		return true;

	return qx_runtime_scheduler_owner_slot(taskoid, slot_count) == slot_index;
}

static int32
qx_runtime_retry_backoff_ms(const char *priority,
							int32 retry_count,
							int32 lease_ttl_ms,
							Oid taskoid)
{
	int32		base_ms;
	int32		retry_factor;
	int32		jitter_window_ms;
	int32		jitter_ms;
	uint32		jitter_seed;

	base_ms = Max(lease_ttl_ms, QX_SCHEDULER_WORKER_NAPTIME_MS);

	if (priority != NULL && priority[0] != '\0')
	{
		if (pg_strcasecmp(priority, "urgent") == 0 ||
			pg_strcasecmp(priority, "critical") == 0)
			base_ms = Max(base_ms / 2, QX_SCHEDULER_WORKER_NAPTIME_MS);
		else if (pg_strcasecmp(priority, "high") == 0)
			base_ms = Max((base_ms * 3) / 4, QX_SCHEDULER_WORKER_NAPTIME_MS * 2);
		else if (pg_strcasecmp(priority, "normal") == 0)
			base_ms = Max(base_ms, QX_SCHEDULER_WORKER_NAPTIME_MS * 3);
		else if (pg_strcasecmp(priority, "low") == 0)
			base_ms = Max(base_ms * 2, QX_SCHEDULER_WORKER_NAPTIME_MS * 5);
		else if (pg_strcasecmp(priority, "idle") == 0)
			base_ms = Max(base_ms * 3, QX_SCHEDULER_WORKER_NAPTIME_MS * 8);
	}

	retry_factor = 1 << Min(Max(retry_count, 1) - 1, 3);
	base_ms = Min(base_ms * retry_factor, 60000);

	jitter_window_ms = Min(Max(base_ms / 8, 1), 1500);
	jitter_seed = (uint32) taskoid ^ ((uint32) Max(retry_count, 1) * UINT32_C(2654435761));
	jitter_ms = (int32) (jitter_seed % (uint32) jitter_window_ms);

	return base_ms + jitter_ms;
}

static bool
qx_runtime_retry_candidate_better(
	const QxCatalogTaskInfo *candidate_task,
	const QxCatalogAttemptInfo *candidate_attempt,
	const QxSchedulerQueueSnapshot *candidate_queue,
	const QxCatalogTaskInfo *best_task,
	const QxCatalogAttemptInfo *best_attempt,
	const QxSchedulerQueueSnapshot *best_queue)
{
	int32		candidate_weight;
	int32		best_weight;

	Assert(candidate_task != NULL);
	Assert(candidate_attempt != NULL);
	Assert(candidate_queue != NULL);

	if (best_task == NULL || best_attempt == NULL || best_queue == NULL)
		return true;

	candidate_weight = QxSchedulerPriorityWeight(candidate_queue->priority);
	best_weight = QxSchedulerPriorityWeight(best_queue->priority);

	if (candidate_weight != best_weight)
		return candidate_weight > best_weight;

	if (candidate_queue->eligible_at != best_queue->eligible_at)
		return candidate_queue->eligible_at < best_queue->eligible_at;

	if (candidate_queue->enqueued_at != best_queue->enqueued_at)
		return candidate_queue->enqueued_at < best_queue->enqueued_at;

	if (candidate_attempt->seqno != best_attempt->seqno)
		return candidate_attempt->seqno < best_attempt->seqno;

	return candidate_task->oid < best_task->oid;
}

static void
qx_runtime_set_heartbeat_receipt(QxSchedulerHeartbeatSnapshot *heartbeat,
								 const char *receipt_mode)
{
	if (heartbeat == NULL)
		return;

	if (heartbeat->receipt_mode != NULL)
		pfree(heartbeat->receipt_mode);
	heartbeat->receipt_mode = pstrdup(qx_safe_runtime_text(receipt_mode));
}

static void
qx_runtime_free_scheduler_lease_snapshot(QxSchedulerLeaseSnapshot *snapshot)
{
	QxCatalogFreeSchedulerLeaseSnapshot(snapshot);
}

static List *
qx_runtime_scheduler_db_targets(void)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *targets = NIL;

	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database form = (Form_pg_database) GETSTRUCT(tup);
		QxRuntimeSchedulerDbTarget *target;

		if (!form->datallowconn || form->datistemplate)
			continue;
		if (!qx_runtime_scheduler_db_target_allowed(NameStr(form->datname)))
			continue;

		target = palloc0(sizeof(QxRuntimeSchedulerDbTarget));
		target->dboid = form->oid;
		target->dbname = pstrdup(NameStr(form->datname));
		targets = lappend(targets, target);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return targets;
}

static bool
qx_runtime_scheduler_db_target_allowed(const char *dbname)
{
	/*
	 * pg_regress temp clusters create short-lived databases such as
	 * regression_utf8 and then assert DROP DATABASE/TABLESPACE cleanup.
	 * Persistent scheduler connections should not attach to those transient
	 * core-test databases; the QX regression itself runs in "regression".
	 */
	if (DataDir != NULL && strstr(DataDir, "testrun") != NULL)
		return dbname != NULL && strcmp(dbname, "regression") == 0;

	return true;
}

static void
qx_runtime_free_scheduler_db_targets(List *targets)
{
	ListCell   *lc;

	foreach(lc, targets)
	{
		QxRuntimeSchedulerDbTarget *target = lfirst(lc);

		if (target->dbname != NULL)
			pfree(target->dbname);
		pfree(target);
	}

	list_free(targets);
}

static QxRuntimeSchedulerDbWorker *
qx_runtime_find_scheduler_db_worker(const List *workers,
									Oid dboid,
									int32 slot_index)
{
	ListCell   *lc;

	foreach(lc, workers)
	{
		QxRuntimeSchedulerDbWorker *worker = lfirst(lc);

		if (worker->dboid == dboid &&
			worker->slot_index == slot_index)
			return worker;
	}

	return NULL;
}

static bool
qx_runtime_launch_scheduler_db_worker(Oid dboid,
									  const char *dbname,
									  int32 slot_index,
									  int32 slot_count,
									  BackgroundWorkerHandle **handle)
{
	BackgroundWorker worker;
	QxRuntimeSchedulerWorkerKey key;
	MemoryContext oldcontext;

	memset(&worker, 0, sizeof(worker));
	memset(&key, 0, sizeof(key));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 1;
	snprintf(worker.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(worker.bgw_function_name, BGW_MAXLEN,
			 "QxRuntimeSchedulerDatabaseWorkerMain");
	snprintf(worker.bgw_name, BGW_MAXLEN,
			 "qhapaqxian scheduler db %s slot %d/%d",
			 dbname != NULL ? dbname : "<unknown>",
			 slot_index + 1,
			 Max(slot_count, 1));
	snprintf(worker.bgw_type, BGW_MAXLEN, "qhapaqxian scheduler db");
	worker.bgw_main_arg = ObjectIdGetDatum(dboid);
	key.dboid = dboid;
	key.slot_index = slot_index;
	key.slot_count = slot_count;
	memcpy(worker.bgw_extra, &key, sizeof(key));
	worker.bgw_notify_pid = MyProcPid;

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	if (!RegisterDynamicBackgroundWorker(&worker, handle))
	{
		MemoryContextSwitchTo(oldcontext);
		return false;
	}
	MemoryContextSwitchTo(oldcontext);
	return true;
}

static void
qx_runtime_scheduler_heartbeat_wait(uint32 *wait_event,
									const char *wait_name,
									long timeout_ms)
{
	if (*wait_event == 0)
		*wait_event = WaitEventExtensionNew(wait_name);

	(void) WaitLatch(MyLatch,
					 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					 timeout_ms,
					 *wait_event);
	ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}
}

static bool
qx_runtime_reclaim_running_attempt(Relation taskrel,
								   Relation attemptrel,
								   Relation queueledgerrel,
								   Relation leaseledgerrel,
								   Relation heartbeatledgerrel,
								   const QxCatalogTaskInfo *task,
								   const QxCatalogAttemptInfo *attempt,
								   const QxSchedulerLeaseSnapshot *lease_snapshot,
								   Oid queueoid,
								   const char *worker_name,
								   QxRuntimeSchedulerCycleStats *stats)
{
	QxSchedulerTaskEnvelope scheduler_envelope;
	QxSchedulerQueueSnapshot *queue_snapshot = NULL;
	QxSchedulerLeaseSnapshot *reclaimed_lease_snapshot;
	QxSchedulerHeartbeatSnapshot *heartbeat_snapshot;
	char	   *selected_contract = NULL;
	char	   *checkpoint_label = NULL;
	char	   *resume_label = NULL;
	bool		repairable;
	int32		retry_backoff_ms;
	int32		heartbeat_lag_ms;
	Oid			reclaimed_queueoid = queueoid;
	Oid			leaseoid;

	Assert(task != NULL);
	Assert(attempt != NULL);
	Assert(lease_snapshot != NULL);

	repairable = OidIsValid(task->lastcheckpointid);

	if (repairable)
	{
		checkpoint_label =
			qx_runtime_recovery_checkpoint_label(task->lastcheckpointid);
		if (checkpoint_label != NULL)
			resume_label = pstrdup(checkpoint_label);
		selected_contract =
			qx_runtime_recovery_selected_contract(task, true);
		qx_runtime_fill_recovery_scheduler_envelope(&scheduler_envelope,
													(task),
													attempt->oid,
													Max((int32) attempt->seqno, 0),
													checkpoint_label,
													resume_label,
													true,
													true,
													(attempt->seqno > 1),
													selected_contract);
		queue_snapshot = QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
		queue_snapshot->queue_kind = QX_SCHEDULER_QUEUE_RECOVERY;
		queue_snapshot->retry_count = Max((int32) attempt->seqno, 0);
		reclaimed_queueoid = QxCatalogInsertSchedulerQueue(queueledgerrel,
													   queue_snapshot);
		CommandCounterIncrement();
		if (stats != NULL)
			stats->recovery_requeued++;

		QxCatalogUpdateAttemptState(attemptrel, attempt->oid,
								QX_ATTEMPT_STATE_CHECKPOINTED);
		QxCatalogUpdateTaskRuntime(taskrel, task->oid,
							   QX_TASK_STATE_CHECKPOINTED,
							   attempt->oid,
							   true,
							   task->lastcheckpointid,
							   true);
		CommandCounterIncrement();
		if (stats != NULL)
			stats->leases_repaired++;
	}
	else
	{
		selected_contract =
			qx_runtime_recovery_selected_contract(task, false);
		qx_runtime_fill_recovery_scheduler_envelope(&scheduler_envelope,
													(task),
													attempt->oid,
													Max((int32) attempt->seqno, 1),
													NULL,
													NULL,
													false,
													true,
													true,
													selected_contract);
		queue_snapshot = QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
		queue_snapshot->queue_kind = QX_SCHEDULER_QUEUE_RETRY;
		queue_snapshot->retry_count = Max((int32) attempt->seqno, 1);
		retry_backoff_ms = qx_runtime_retry_backoff_ms(queue_snapshot->priority,
													   queue_snapshot->retry_count,
													   qx_runtime_lease_ttl_ms(lease_snapshot),
													   task->oid);
		queue_snapshot->eligible_at = queue_snapshot->enqueued_at +
			((TimestampTz) retry_backoff_ms * USECS_PER_MSEC);
		reclaimed_queueoid = QxCatalogInsertSchedulerQueue(queueledgerrel,
													   queue_snapshot);
		CommandCounterIncrement();
		if (stats != NULL)
			stats->recovery_requeued++;

		QxCatalogUpdateAttemptState(attemptrel, attempt->oid,
								QX_ATTEMPT_STATE_FAILED);
		QxCatalogUpdateTaskRuntime(taskrel, task->oid,
							   QX_TASK_STATE_QUEUED,
							   attempt->oid,
							   true,
							   InvalidOid,
							   true);
		CommandCounterIncrement();
		if (stats != NULL)
			stats->leases_repaired++;
	}

	reclaimed_lease_snapshot =
		QxSchedulerReleaseLeaseSnapshot(lease_snapshot, true);
	if (worker_name != NULL && worker_name[0] != '\0')
	{
		if (reclaimed_lease_snapshot->worker_name != NULL)
			pfree(reclaimed_lease_snapshot->worker_name);
		reclaimed_lease_snapshot->worker_name = pstrdup(worker_name);
	}
	leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel,
										 reclaimed_queueoid,
										 reclaimed_lease_snapshot);
	heartbeat_snapshot =
		QxSchedulerFinalizeHeartbeatSnapshot(reclaimed_lease_snapshot,
											 "scheduler-reclaim");
	heartbeat_lag_ms = Max(qx_runtime_lease_ttl_ms(lease_snapshot) / 3, 1);
	heartbeat_snapshot->state = QX_SCHEDULER_HEARTBEAT_MISSED;
	heartbeat_snapshot->lag_ms = heartbeat_lag_ms;
	heartbeat_snapshot->stale = true;
	(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel,
										 leaseoid,
										 heartbeat_snapshot);
	CommandCounterIncrement();

	if (stats != NULL)
	{
		stats->leases_reclaimed++;
		stats->heartbeats_written++;
	}
	QxStatReportSchedulerEvent(MyDatabaseId, "reclaim");
	qx_backend_supervisor_fence_stale(task->oid);

	if (selected_contract != NULL)
		pfree(selected_contract);
	if (checkpoint_label != NULL)
		pfree(checkpoint_label);
	if (resume_label != NULL)
		pfree(resume_label);

	return repairable;
}

static void
qx_runtime_run_scheduler_cycle(QxRuntimeSchedulerCycleStats *stats,
							   int32 slot_index,
							   int32 slot_count)
{
	List	   *tasks;
	ListCell   *lc;
	Relation	taskrel;
	Relation	attemptrel;
	Relation	queueledgerrel;
	Relation	leaseledgerrel;
	Relation	heartbeatledgerrel;
	TimestampTz	now;

	Assert(stats != NULL);
	MemSet(stats, 0, sizeof(*stats));

	tasks = QxCatalogBuildTaskInfoList(MyDatabaseId, InvalidOid);
	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	queueledgerrel = table_open(QxSchedulerQueueRelationId, RowExclusiveLock);
	leaseledgerrel = table_open(QxSchedulerLeaseRelationId, RowExclusiveLock);
	heartbeatledgerrel = table_open(QxSchedulerHeartbeatRelationId,
									RowExclusiveLock);
	now = GetCurrentTimestamp();

	foreach(lc, tasks)
	{
		QxCatalogTaskInfo *task = lfirst(lc);
		QxCatalogAttemptInfo attempt;
		QxSchedulerLeaseSnapshot lease_snapshot;
		QxSchedulerLeaseSnapshot *renewed_lease_snapshot;
		QxSchedulerHeartbeatSnapshot *heartbeat_snapshot;
		Oid			queueoid = InvalidOid;
		Oid			leaseoid;
		int32		lease_ttl_ms;

		stats->tasks_scanned++;
		if (task->state != QX_TASK_STATE_RUNNING ||
			!OidIsValid(task->lastattemptid))
			continue;
		if (!qx_runtime_scheduler_worker_owns_task(task->oid,
												   slot_index,
												   slot_count))
		{
			stats->ownership_skipped++;
			continue;
		}

		stats->running_tasks++;
		if (!QxCatalogLookupAttemptByOid(task->lastattemptid, &attempt))
			elog(ERROR, "cache lookup failed for QhapaqXian attempt %u",
				 task->lastattemptid);

		if (attempt.state != QX_ATTEMPT_STATE_RUNNING)
		{
			QxCatalogFreeAttemptInfo(&attempt);
			continue;
		}

		MemSet(&lease_snapshot, 0, sizeof(lease_snapshot));
		if (!QxCatalogLookupLatestSchedulerLease(MyDatabaseId, InvalidOid,
									 task->oid,
									 attempt.oid,
									 &lease_snapshot,
									 &queueoid))
		{
			QxCatalogFreeAttemptInfo(&attempt);
			continue;
		}

		stats->leases_seen++;
		if (lease_snapshot.state != QX_SCHEDULER_LEASE_HELD ||
			!QxSchedulerLeaseNeedsReclaim(&lease_snapshot, now))
		{
			qx_runtime_free_scheduler_lease_snapshot(&lease_snapshot);
			QxCatalogFreeAttemptInfo(&attempt);
			continue;
		}

		if (lease_snapshot.renewal_count <= 0)
		{
			lease_ttl_ms = qx_runtime_lease_ttl_ms(&lease_snapshot);
			renewed_lease_snapshot =
				QxSchedulerRenewLeaseSnapshot(&lease_snapshot, lease_ttl_ms);
			leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel,
												 queueoid,
												 renewed_lease_snapshot);
			heartbeat_snapshot =
				QxSchedulerHeartbeatSnapshotFromLease(renewed_lease_snapshot);
			qx_runtime_set_heartbeat_receipt(heartbeat_snapshot,
											 "scheduler-renew");
			(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel,
												 leaseoid,
												 heartbeat_snapshot);
			CommandCounterIncrement();

			stats->leases_renewed++;
			stats->heartbeats_written++;
			QxStatReportSchedulerEvent(MyDatabaseId, "renew");
		}
		else
			(void) qx_runtime_reclaim_running_attempt(taskrel,
													 attemptrel,
													 queueledgerrel,
													 leaseledgerrel,
													 heartbeatledgerrel,
													 task,
													 &attempt,
													 &lease_snapshot,
													 queueoid,
													 MyBgworkerEntry != NULL ?
													 MyBgworkerEntry->bgw_name :
													 "qhapaqxian scheduler",
													 stats);

		qx_runtime_free_scheduler_lease_snapshot(&lease_snapshot);
		QxCatalogFreeAttemptInfo(&attempt);
	}

	table_close(heartbeatledgerrel, RowExclusiveLock);
	table_close(leaseledgerrel, RowExclusiveLock);
	table_close(queueledgerrel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);
	QxCatalogFreeTaskInfoList(tasks);
}

static void
qx_runtime_run_scheduler_retry_cycle(QxRuntimeSchedulerCycleStats *stats,
									 int32 slot_index,
									 int32 slot_count)
{
	List	   *tasks;
	ListCell   *lc;
	Relation	taskrel;
	Relation	attemptrel;
	Relation	steprel;
	Relation	eventrel;
	Relation	tracerel;
	Relation	checkpointrel;
	Relation	queueledgerrel;
	Relation	leaseledgerrel;
	Relation	heartbeatledgerrel;
	TimestampTz	now;
	QxCatalogTaskInfo *selected_task = NULL;
	QxCatalogAttemptInfo selected_failed_attempt;
	QxSchedulerQueueSnapshot selected_retry_queue;
	bool		have_selected = false;

	tasks = QxCatalogBuildTaskInfoList(MyDatabaseId, InvalidOid);
	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	steprel = table_open(QxStepRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);
	checkpointrel = table_open(QxCheckpointRelationId, RowExclusiveLock);
	queueledgerrel = table_open(QxSchedulerQueueRelationId, RowExclusiveLock);
	leaseledgerrel = table_open(QxSchedulerLeaseRelationId, RowExclusiveLock);
	heartbeatledgerrel = table_open(QxSchedulerHeartbeatRelationId,
									RowExclusiveLock);
	now = GetCurrentTimestamp();
	MemSet(&selected_failed_attempt, 0, sizeof(selected_failed_attempt));
	MemSet(&selected_retry_queue, 0, sizeof(selected_retry_queue));

	foreach(lc, tasks)
	{
		QxCatalogTaskInfo *task = lfirst(lc);
		QxCatalogAttemptInfo failed_attempt;
		QxSchedulerQueueSnapshot retry_queue;

		if (task->state != QX_TASK_STATE_QUEUED ||
			!OidIsValid(task->lastattemptid) ||
			OidIsValid(task->lastcheckpointid))
			continue;
		if (!qx_runtime_scheduler_worker_owns_task(task->oid,
												   slot_index,
												   slot_count))
		{
			if (stats != NULL)
				stats->ownership_skipped++;
			continue;
		}

		if (!QxCatalogLookupAttemptByOid(task->lastattemptid, &failed_attempt))
			elog(ERROR, "cache lookup failed for QhapaqXian attempt %u",
				 task->lastattemptid);

		if (failed_attempt.state != QX_ATTEMPT_STATE_FAILED)
		{
			QxCatalogFreeAttemptInfo(&failed_attempt);
			continue;
		}

		MemSet(&retry_queue, 0, sizeof(retry_queue));
		if (!QxCatalogLookupLatestSchedulerQueue(MyDatabaseId, InvalidOid,
									 task->oid,
									 failed_attempt.oid,
									 &retry_queue))
		{
			QxCatalogFreeAttemptInfo(&failed_attempt);
			continue;
		}

		if (retry_queue.queue_kind != QX_SCHEDULER_QUEUE_RETRY ||
			retry_queue.runnable_count <= 0 ||
			retry_queue.blocked_count > 0)
		{
			qx_runtime_free_scheduler_queue_snapshot(&retry_queue);
			QxCatalogFreeAttemptInfo(&failed_attempt);
			continue;
		}

		if (now < retry_queue.eligible_at)
		{
			if (stats != NULL)
			{
				stats->queued_retry_tasks++;
				stats->retry_waiting_tasks++;
				stats->last_retry_backoff_ms =
					(int32) ((retry_queue.eligible_at -
							  retry_queue.enqueued_at) / USECS_PER_MSEC);
				stats->max_retry_backoff_ms =
					Max(stats->max_retry_backoff_ms,
						stats->last_retry_backoff_ms);
			}
			qx_runtime_free_scheduler_queue_snapshot(&retry_queue);
			QxCatalogFreeAttemptInfo(&failed_attempt);
			continue;
		}

		if (stats != NULL)
		{
			stats->queued_retry_tasks++;
			stats->retry_ready_tasks++;
			stats->last_retry_backoff_ms =
				(int32) ((retry_queue.eligible_at -
						  retry_queue.enqueued_at) / USECS_PER_MSEC);
			stats->max_retry_backoff_ms =
				Max(stats->max_retry_backoff_ms,
					stats->last_retry_backoff_ms);
		}

		if (retry_queue.retry_count >= qx_runtime_scheduler_max_retries())
		{
			(void) qx_runtime_dead_letter_retry_task(taskrel,
													 attemptrel,
													 eventrel,
													 tracerel,
													 queueledgerrel,
													 task,
													 &failed_attempt,
													 &retry_queue,
													 MyBgworkerEntry != NULL ?
													 MyBgworkerEntry->bgw_name :
													 "qhapaqxian scheduler",
													 stats);
			qx_runtime_free_scheduler_queue_snapshot(&retry_queue);
			QxCatalogFreeAttemptInfo(&failed_attempt);
			continue;
		}

		if (!have_selected ||
			qx_runtime_retry_candidate_better(task,
											 &failed_attempt,
											 &retry_queue,
											 selected_task,
											 &selected_failed_attempt,
											 &selected_retry_queue))
		{
			if (have_selected)
			{
				qx_runtime_free_scheduler_queue_snapshot(&selected_retry_queue);
				QxCatalogFreeAttemptInfo(&selected_failed_attempt);
			}

			selected_task = task;
			selected_failed_attempt = failed_attempt;
			selected_retry_queue = retry_queue;
			MemSet(&failed_attempt, 0, sizeof(failed_attempt));
			MemSet(&retry_queue, 0, sizeof(retry_queue));
			have_selected = true;
		}

		qx_runtime_free_scheduler_queue_snapshot(&retry_queue);
		QxCatalogFreeAttemptInfo(&failed_attempt);
	}

	if (have_selected)
	{
		(void) qx_runtime_dispatch_retry_task(taskrel,
											  attemptrel,
											  steprel,
											  eventrel,
											  tracerel,
											  checkpointrel,
											  queueledgerrel,
											  leaseledgerrel,
											  heartbeatledgerrel,
											  selected_task,
											  &selected_failed_attempt,
											  &selected_retry_queue,
											  MyBgworkerEntry != NULL ?
											  MyBgworkerEntry->bgw_name :
											  "qhapaqxian scheduler",
											  stats);
		qx_runtime_free_scheduler_queue_snapshot(&selected_retry_queue);
		QxCatalogFreeAttemptInfo(&selected_failed_attempt);
		if (stats != NULL && stats->retry_ready_tasks > 0)
			stats->retry_waiting_tasks += stats->retry_ready_tasks - 1;
	}

	table_close(heartbeatledgerrel, RowExclusiveLock);
	table_close(leaseledgerrel, RowExclusiveLock);
	table_close(queueledgerrel, RowExclusiveLock);
	table_close(checkpointrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);
	table_close(eventrel, RowExclusiveLock);
	table_close(steprel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);
	QxCatalogFreeTaskInfoList(tasks);
}

static bool
qx_runtime_dead_letter_retry_task(Relation taskrel,
								  Relation attemptrel,
								  Relation eventrel,
								  Relation tracerel,
								  Relation queueledgerrel,
								  const QxCatalogTaskInfo *task,
								  const QxCatalogAttemptInfo *failed_attempt,
								  const QxSchedulerQueueSnapshot *retry_queue,
								  const char *worker_name,
								  QxRuntimeSchedulerCycleStats *stats)
{
	QxSchedulerQueueSnapshot dead_queue;
	TimestampTz	now;
	char	   *payload;
	Oid			queueoid;

	Assert(task != NULL);
	Assert(failed_attempt != NULL);
	Assert(retry_queue != NULL);

	if (task->state != QX_TASK_STATE_QUEUED ||
		failed_attempt->state != QX_ATTEMPT_STATE_FAILED)
		return false;

	now = GetCurrentTimestamp();
	dead_queue = *retry_queue;
	dead_queue.queue_kind = QX_SCHEDULER_QUEUE_MAINTENANCE;
	dead_queue.runnable_count = 0;
	dead_queue.leased_count = 0;
	dead_queue.blocked_count = 1;
	dead_queue.eligible_at = now;
	dead_queue.updated_at = now;
	queueoid = QxCatalogInsertSchedulerQueue(queueledgerrel, &dead_queue);
	CommandCounterIncrement();

	QxCatalogUpdateAttemptState(attemptrel, failed_attempt->oid,
							QX_ATTEMPT_STATE_FAILED);
	QxCatalogUpdateTaskRuntime(taskrel, task->oid,
						   QX_TASK_STATE_FAILED,
						   failed_attempt->oid,
						   true,
						   InvalidOid,
						   true);

	payload = psprintf("retry_count=%d;max_retries=%d;queueoid=%u;worker=%s;task_state=%c;queue_kind=%d",
					   retry_queue->retry_count,
					   qx_runtime_scheduler_max_retries(),
					   queueoid,
					   worker_name != NULL ? worker_name : "qhapaqxian scheduler",
					   QX_TASK_STATE_FAILED,
					   (int) QX_SCHEDULER_QUEUE_MAINTENANCE);
	payload = qx_runtime_append_recovery_payload(payload);
	(void) QxCatalogInsertEvent(eventrel, task->sessionoid, task->oid, InvalidOid,
						   task->ownerid, NULL, "TASK_DEAD_LETTERED", payload);
	(void) QxCatalogInsertTrace(tracerel, task->sessionoid, task->oid, InvalidOid,
						   task->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.dead_letter", payload);
	pfree(payload);

	if (stats != NULL)
		stats->dead_lettered_tasks++;

	return true;
}

static bool
qx_runtime_dispatch_retry_task(Relation taskrel,
							   Relation attemptrel,
							   Relation steprel,
							   Relation eventrel,
							   Relation tracerel,
							   Relation checkpointrel,
							   Relation queueledgerrel,
							   Relation leaseledgerrel,
							   Relation heartbeatledgerrel,
							   const QxCatalogTaskInfo *task,
							   const QxCatalogAttemptInfo *failed_attempt,
							   const QxSchedulerQueueSnapshot *retry_queue,
							   const char *worker_name,
							   QxRuntimeSchedulerCycleStats *stats)
{
	List	   *authorized_contracts = NIL;
	char	   *selected_contract;
	QxSchedulerTaskEnvelope scheduler_envelope;
	QxSchedulerQueueSnapshot *dispatch_queue_snapshot;
	QxSchedulerLeaseSnapshot *lease_snapshot;
	QxSchedulerHeartbeatSnapshot *heartbeat_snapshot;
	QxSchedulerLeaseSnapshot *released_lease_snapshot;
	QxSchedulerHeartbeatSnapshot *final_heartbeat_snapshot;
	QxExternalToolResult tool_result;
	QxSemanticExecutionMetadata semantic_meta;
	char	   *payload;
	char	   *checkpoint_data;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			checkpointoid;
	Oid			queueoid;
	Oid			leaseoid;
	int16		nextattemptseqno;
	int16		stepseqbase;

	Assert(task != NULL);
	Assert(failed_attempt != NULL);
	Assert(retry_queue != NULL);

	if (task->state != QX_TASK_STATE_QUEUED ||
		failed_attempt->state != QX_ATTEMPT_STATE_FAILED)
		return false;

	authorized_contracts = qx_parse_task_authorized_contracts(task->authorized_tools);
	selected_contract = qx_runtime_task_phase_contract(task, false);
	if (selected_contract == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no authorized retry contract", task->oid)));

	nextattemptseqno = failed_attempt->seqno + 1;
	stepseqbase = QxCatalogMaxStepSeqnoForTask(MyDatabaseId, task->oid);
	memset(&tool_result, 0, sizeof(tool_result));

	attemptoid = QxCatalogInsertAttempt(attemptrel,
								   task->sessionoid,
								   task->oid,
								   task->ownerid,
								   InvalidOid,
								   nextattemptseqno,
								   QX_ATTEMPT_STATE_RUNNING,
								   "retry");
	CommandCounterIncrement();

	QxCatalogUpdateTaskRuntime(taskrel, task->oid, QX_TASK_STATE_RUNNING,
						   attemptoid, true, InvalidOid, false);

	qx_scheduler_fill_envelope(&scheduler_envelope,
							   task->ownerid,
							   task->sessionoid,
							   task->agentoid,
							   task->identityoid,
							   task->namespacepolicyoid,
							   task->oid,
							   attemptoid,
							   task->name,
							   task->agent_name,
							   task->identity_name,
							   task->policy_name,
							   task->priority,
							   "stage8.after_capture",
							   NULL,
							   false,
							   true,
							   true,
							   task->budget_tokens,
							   task->budget_cost,
							   task->estimated_tokens,
							   task->estimated_cost,
							   nextattemptseqno - 1,
							   selected_contract);
	dispatch_queue_snapshot =
		QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
	dispatch_queue_snapshot->queue_kind = QX_SCHEDULER_QUEUE_RETRY;
	dispatch_queue_snapshot->retry_count = nextattemptseqno - 1;
	queueoid = QxCatalogInsertSchedulerQueue(queueledgerrel, dispatch_queue_snapshot);
	CommandCounterIncrement();

	payload = psprintf("retry_from_attempt=%d;retry_delay_ms=" INT64_FORMAT,
					   failed_attempt->seqno,
					   (int64) ((retry_queue->eligible_at - retry_queue->enqueued_at) /
								USECS_PER_MSEC));
	payload = qx_runtime_append_recovery_payload(payload);
	payload = qx_scheduler_append_runtime_payload(payload,
												  &scheduler_envelope,
												  false);
	(void) QxCatalogInsertEvent(eventrel, task->sessionoid, task->oid, InvalidOid,
						   task->ownerid, NULL, "TASK_RETRIED", payload);
	(void) QxCatalogInsertTrace(tracerel, task->sessionoid, task->oid, InvalidOid,
						   task->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.retry", payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, task->sessionoid, task->oid, stepseqbase,
							 "stage8.retry_dispatch",
							 "Attempt retried execution after stale no-checkpoint reclaim");
	QxCatalogChargeTaskBudget(taskrel, task->oid, 8, 14, "retry_dispatch");
	payload = psprintf("attempt_opened;state=%c;retry_from_attempt=%d;identity=%s;namespace_policy=%s;tools=%d;budget_cost=%d;budget_tokens=%d",
					   QX_TASK_STATE_RUNNING,
					   failed_attempt->seqno,
					   task->identity_name != NULL ? task->identity_name : "<unknown>",
					   task->policy_name != NULL ? task->policy_name : "<unknown>",
					   list_length(authorized_contracts),
					   task->budget_cost,
					   task->budget_tokens);
	payload = qx_runtime_append_recovery_payload(payload);
	payload = qx_scheduler_append_runtime_payload(payload,
												  &scheduler_envelope,
												  true);
	lease_snapshot = QxSchedulerLeaseSnapshotFromEnvelope(&scheduler_envelope,
														 InvalidOid,
														 worker_name != NULL ?
														 worker_name :
														 "embedded-runtime");
	leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel, queueoid,
										 lease_snapshot);
	heartbeat_snapshot = QxSchedulerHeartbeatSnapshotFromLease(lease_snapshot);
	(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel, leaseoid,
										 heartbeat_snapshot);
	CommandCounterIncrement();
	(void) QxCatalogInsertEvent(eventrel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, NULL, "TASK_RETRY_DISPATCHED", payload);
	(void) QxCatalogInsertTrace(tracerel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.retry_dispatch", payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, task->sessionoid, task->oid,
							 stepseqbase + 1,
							 "stage15.authorize_tools",
							 "Retry dispatch reused the stored runtime tool allowlist");
	payload = psprintf("namespace_policy=%s;authorized_tools=%d;tool_tokens=%d;tool_cost=%d;retry_from_attempt=%d",
					   task->policy_name != NULL ? task->policy_name : "<unknown>",
					   list_length(authorized_contracts),
					   task->authorized_tool_tokens,
					   task->authorized_tool_cost,
					   failed_attempt->seqno);
	(void) QxCatalogInsertEvent(eventrel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, NULL, "TASK_TOOLS_AUTHORIZED", payload);
	(void) QxCatalogInsertTrace(tracerel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.authorize_tools", payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, task->sessionoid, task->oid,
							 stepseqbase + 2,
							 "stage16.external_submit",
							 "Retry attempt executed the selected tool through its principal program");
	qx_execute_tool_contract(selected_contract, "submit", task->oid,
							 task->goal != NULL ? task->goal : "",
							 task->input != NULL,
							 &tool_result);
	qx_build_semantic_execution_metadata(&semantic_meta, &tool_result);
	QxCatalogChargeTaskBudget(taskrel, task->oid,
						  tool_result.token_charge,
						  tool_result.cost_charge,
						  "external_submit");
	payload = qx_format_external_execution_payload("submit", &tool_result);
	(void) QxCatalogInsertEvent(eventrel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, &semantic_meta,
						   "TASK_TOOL_EXECUTED", payload);
	(void) QxCatalogInsertTrace(tracerel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, &semantic_meta, QX_TRACE_STATE_CLOSED,
						   "runtime.external_submit", payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, task->sessionoid, task->oid,
							 stepseqbase + 3,
							 "stage8.capture_input",
							 "Retry attempt captured task goal and raw input");
	QxCatalogChargeTaskBudget(taskrel, task->oid,
						  task->input != NULL ? 24 : 8,
						  13,
						  "capture_input");
	payload = psprintf("input_present=%s;task_name=%s",
					   task->input != NULL ? "true" : "false",
					   task->name != NULL ? task->name : "<anonymous>");
	(void) QxCatalogInsertEvent(eventrel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, NULL, "TASK_INPUT_CAPTURED", payload);
	(void) QxCatalogInsertTrace(tracerel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.capture_input", payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, task->sessionoid, task->oid,
							 stepseqbase + 4,
							 "stage8.checkpoint_barrier",
							 "Retry attempt reached a resumable checkpoint barrier");
	QxCatalogChargeTaskBudget(taskrel, task->oid, 4, 14, "checkpoint_barrier");
	payload = psprintf("checkpoint=stage8.after_capture;task_state=%c",
					   QX_TASK_STATE_CHECKPOINTED);
	(void) QxCatalogInsertEvent(eventrel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, &semantic_meta,
						   "TASK_CHECKPOINTED", payload);
	(void) QxCatalogInsertTrace(tracerel, task->sessionoid, task->oid, stepoid,
						   task->ownerid, &semantic_meta, QX_TRACE_STATE_CLOSED,
						   "runtime.checkpoint", payload);

	checkpoint_data = psprintf("task=%u;attempt=%u;session=%u;next_step=%d",
							   task->oid,
							   attemptoid,
							   task->sessionoid,
							   stepseqbase + 5);
	checkpointoid = QxCatalogInsertCheckpoint(checkpointrel,
										 task->sessionoid,
										 task->oid,
										 attemptoid,
										 stepoid,
										 task->ownerid,
										 &semantic_meta,
										 QX_TASK_STATE_CHECKPOINTED,
										 stepseqbase + 5,
										 "stage8.after_capture",
										 checkpoint_data);
	pfree(checkpoint_data);

	QxCatalogUpdateAttemptState(attemptrel, attemptoid, QX_ATTEMPT_STATE_CHECKPOINTED);
	QxCatalogUpdateTaskRuntime(taskrel, task->oid, QX_TASK_STATE_CHECKPOINTED,
						   attemptoid, true, checkpointoid, true);
	released_lease_snapshot = QxSchedulerReleaseLeaseSnapshot(lease_snapshot,
															 false);
	leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel, queueoid,
										 released_lease_snapshot);
	final_heartbeat_snapshot =
		QxSchedulerFinalizeHeartbeatSnapshot(released_lease_snapshot,
											 "scheduler-release");
	(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel, leaseoid,
										 final_heartbeat_snapshot);
	CommandCounterIncrement();
	QxStatReportSchedulerEvent(MyDatabaseId, "release");

	(void) QxCatalogInsertEvent(eventrel, task->sessionoid, task->oid, InvalidOid,
						   task->ownerid, NULL,
						   "TASK_READY_FOR_RESUME", payload);
	(void) QxCatalogInsertTrace(tracerel, task->sessionoid, task->oid, InvalidOid,
						   task->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.pause",
						   "Task paused at durable checkpoint and awaits RESUME TASK");
	pfree(payload);
	qx_free_external_result(&tool_result);
	if (selected_contract != NULL)
		pfree(selected_contract);
	if (authorized_contracts != NIL)
		list_free_deep(authorized_contracts);

	if (stats != NULL)
	{
		stats->retry_dispatches++;
		stats->heartbeats_written += 2;
	}

	return true;
}

static void
qx_runtime_log_scheduler_cycle(const char *worker_name,
							   const QxRuntimeSchedulerCycleStats *stats)
{
	if (stats == NULL)
		return;

	if (stats->leases_renewed <= 0 &&
		stats->leases_reclaimed <= 0 &&
		stats->leases_repaired <= 0 &&
		stats->recovery_requeued <= 0 &&
		stats->retry_dispatches <= 0 &&
		stats->dead_lettered_tasks <= 0 &&
		stats->retry_waiting_tasks <= 0 &&
		stats->ownership_skipped <= 0)
		return;

	elog(DEBUG1,
		 "%s: tasks=%d running=%d queued_retry=%d retry_ready=%d retry_waiting=%d skipped=%d leases=%d renewed=%d reclaimed=%d repaired=%d requeued=%d retry_dispatches=%d dead_lettered=%d last_backoff_ms=%d max_backoff_ms=%d heartbeats=%d",
		 worker_name != NULL ? worker_name : "qhapaqxian scheduler",
		 stats->tasks_scanned,
		 stats->running_tasks,
		 stats->queued_retry_tasks,
		 stats->retry_ready_tasks,
		 stats->retry_waiting_tasks,
		 stats->ownership_skipped,
		 stats->leases_seen,
		 stats->leases_renewed,
		 stats->leases_reclaimed,
		 stats->leases_repaired,
		 stats->recovery_requeued,
		 stats->retry_dispatches,
		 stats->dead_lettered_tasks,
		 stats->last_retry_backoff_ms,
		 stats->max_retry_backoff_ms,
		 stats->heartbeats_written);
}

static void
qx_runtime_fill_task_insert_params(QxCatalogTaskInsertParams *params,
								   const QxRuntimeTaskRequest *request)
{
	Assert(params != NULL);
	Assert(request != NULL);

	MemSet(params, 0, sizeof(*params));
	params->sessionoid = request->sessionoid;
	params->agentoid = request->agentoid;
	params->identityoid = request->identityoid;
	params->namespace_policy_oid = request->namespace_policy_oid;
	params->ownerid = request->ownerid;
	params->task_name = request->task_name;
	params->goal = request->goal;
	params->input = request->input;
	params->priority = request->priority;
	params->authorized_tools = request->authorized_tools;
	params->submit_contract = qx_runtime_request_phase_contract(request, false);
	params->resume_contract = qx_runtime_request_phase_contract(request, true);
	params->budget_tokens = request->budget_tokens;
	params->budget_cost = request->budget_cost;
	params->authorized_tool_tokens = request->authorized_tool_tokens;
	params->authorized_tool_cost = request->authorized_tool_cost;
	params->estimated_tokens = request->estimated_tokens;
	params->estimated_cost = request->estimated_cost;
}

static int16
qx_fetch_attempt_seqno(Oid attemptoid)
{
	QxCatalogAttemptInfo attempt;
	int16		seqno;

	if (!QxCatalogLookupAttemptByOid(attemptoid, &attempt))
		elog(ERROR, "cache lookup failed for QhapaqXian attempt %u", attemptoid);

	seqno = attempt.seqno;
	QxCatalogFreeAttemptInfo(&attempt);

	return seqno;
}

Datum
pg_qx_test_start_recovery_attempt(PG_FUNCTION_ARGS)
{
	Oid			taskoid = PG_GETARG_OID(0);
	Relation	taskrel;
	Relation	attemptrel;
	Relation	queueledgerrel;
	Relation	leaseledgerrel;
	Relation	heartbeatledgerrel;
	QxCatalogTaskInfo task;
	QxSchedulerTaskEnvelope scheduler_envelope;
	QxSchedulerQueueSnapshot *queue_snapshot;
	QxSchedulerLeaseSnapshot *lease_snapshot;
	QxSchedulerHeartbeatSnapshot *heartbeat_snapshot;
	char	   *selected_contract;
	char	   *checkpoint_label;
	char	   *resume_label;
	Oid			attemptoid;
	Oid			queueoid;
	Oid			leaseoid;
	int16		nextattemptseqno;

	/*
	 * Internal regression hook: leave a real durable "backend crashed after
	 * dispatch" shape without executing an external principal.
	 */
	if (!QxCatalogLookupTaskByOid(taskoid, &task))
		elog(ERROR, "cache lookup failed for QhapaqXian task %u", taskoid);

	if (task.ownerid != GetUserId())
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for QhapaqXian task %u", taskoid)));
	}

	if (task.state != QX_TASK_STATE_CHECKPOINTED &&
		task.state != QX_TASK_STATE_RUNNING)
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u is not checkpointed or running", taskoid),
				 errdetail("The recovery test hook requires a resumable durable task shape.")));
	}

	if (!OidIsValid(task.lastcheckpointid))
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no durable checkpoint", taskoid)));
	}

	if (!OidIsValid(task.lastattemptid))
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no previous attempt", taskoid)));
	}

	nextattemptseqno = qx_fetch_attempt_seqno(task.lastattemptid) + 1;
	selected_contract = qx_runtime_recovery_selected_contract(&task, true);
	checkpoint_label = qx_runtime_recovery_checkpoint_label(task.lastcheckpointid);
	resume_label = checkpoint_label != NULL ? pstrdup(checkpoint_label) : NULL;

	if (selected_contract == NULL)
	{
		if (checkpoint_label != NULL)
			pfree(checkpoint_label);
		if (resume_label != NULL)
			pfree(resume_label);
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no authorized resume contract", taskoid)));
	}

	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	queueledgerrel = table_open(QxSchedulerQueueRelationId, RowExclusiveLock);
	leaseledgerrel = table_open(QxSchedulerLeaseRelationId, RowExclusiveLock);
	heartbeatledgerrel = table_open(QxSchedulerHeartbeatRelationId,
									RowExclusiveLock);

	attemptoid = QxCatalogInsertAttempt(attemptrel,
								   task.sessionoid,
								   taskoid,
								   task.ownerid,
								   task.lastcheckpointid,
								   nextattemptseqno,
								   QX_ATTEMPT_STATE_RUNNING,
								   "resume");
	CommandCounterIncrement();

	QxCatalogUpdateTaskRuntime(taskrel, taskoid, QX_TASK_STATE_RUNNING,
						   attemptoid, true, InvalidOid, false);

	qx_runtime_fill_recovery_scheduler_envelope(&scheduler_envelope,
												&task,
												attemptoid,
												Max((int32) nextattemptseqno - 1, 0),
												checkpoint_label,
												resume_label,
												true,
												true,
												true,
												selected_contract);
	queue_snapshot = QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
	queueoid = QxCatalogInsertSchedulerQueue(queueledgerrel, queue_snapshot);
	CommandCounterIncrement();

	lease_snapshot = QxSchedulerLeaseSnapshotFromEnvelope(&scheduler_envelope,
														  InvalidOid,
														  "embedded-runtime");
	leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel, queueoid,
										 lease_snapshot);
	heartbeat_snapshot = QxSchedulerHeartbeatSnapshotFromLease(lease_snapshot);
	(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel, leaseoid,
										 heartbeat_snapshot);
	CommandCounterIncrement();

	table_close(heartbeatledgerrel, RowExclusiveLock);
	table_close(leaseledgerrel, RowExclusiveLock);
	table_close(queueledgerrel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);

	pfree(selected_contract);
	if (checkpoint_label != NULL)
		pfree(checkpoint_label);
	if (resume_label != NULL)
		pfree(resume_label);
	QxCatalogFreeTaskInfo(&task);

	PG_RETURN_OID(attemptoid);
}

static Oid
qx_runtime_test_start_uncheckpointed_task(Oid sessionoid,
										  const char *priority)
{
	QxCatalogSessionInfo session;
	QxCatalogAgentInfo agent;
	QxBudgetPolicy budget;
	QxToolAuthorization authz;
	QxRuntimeTaskRequest request;
	Relation	taskrel;
	Relation	attemptrel;
	Relation	steprel;
	Relation	eventrel;
	Relation	tracerel;
	Relation	queueledgerrel;
	Relation	leaseledgerrel;
	Relation	heartbeatledgerrel;
	QxSchedulerTaskEnvelope scheduler_envelope;
	QxSchedulerQueueSnapshot *queue_snapshot;
	QxSchedulerLeaseSnapshot *lease_snapshot;
	QxSchedulerHeartbeatSnapshot *heartbeat_snapshot;
	List	   *tools = NIL;
	char	   *agent_name = NULL;
	char	   *payload = NULL;
	const char *selected_contract;
	Oid			taskoid;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			queueoid;
	Oid			leaseoid;

	MemSet(&session, 0, sizeof(session));
	MemSet(&agent, 0, sizeof(agent));
	MemSet(&budget, 0, sizeof(budget));
	MemSet(&authz, 0, sizeof(authz));
	MemSet(&request, 0, sizeof(request));

	if (!QxCatalogLookupSessionByOid(sessionoid, &session))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("session %u does not exist", sessionoid)));

	if (session.ownerid != GetUserId())
	{
		QxCatalogFreeSessionInfo(&session);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for QhapaqXian session %u", sessionoid)));
	}

	if (!QxCatalogLookupAgentByOid(session.agentoid, &agent))
	{
		QxCatalogFreeSessionInfo(&session);
		elog(ERROR, "cache lookup failed for QhapaqXian agent %u",
			 session.agentoid);
	}

	/*
	 * Internal regression hook: create a real durable "task dispatched but no
	 * checkpoint was ever written" shape so scheduler supervision can exercise
	 * the non-resumable stale-attempt path.
	 */
	qx_runtime_ensure_startup_recovery_scan(session.ownerid);

	QxBudgetPolicyFromSerialized(agent.budget, &budget);
	tools = QxDeserializeToolList(agent.tools);
	QxValidateToolList(tools);
	QxAuthorizeToolsForNamespace(session.namespacepolicyoid,
								 agent.namespaceoid,
								 session.ownerid,
								 tools,
								 &authz);

	request.sessionoid = session.oid;
	request.agentoid = session.agentoid;
	request.identityoid = session.identityoid;
	request.namespace_policy_oid = session.namespacepolicyoid;
	request.ownerid = session.ownerid;
	request.task_name = "extract_uncheckpointed";
	request.identity_name = session.identity_name;
	request.namespace_policy_name = session.policy_name;
	request.goal = "leave running without checkpoint";
	request.input = NULL;
	request.priority = qx_effective_priority_name(priority);
	request.authorized_tools = authz.tool_contracts;
	request.authorized_tool_oids = authz.tool_oids;
	request.budget_tokens = budget.has_token_limit ? budget.token_limit : 1024;
	request.budget_cost = budget.has_cost_limit ? budget.cost_limit : 128;
	request.estimated_tokens = Max(authz.tool_token_cost * 4, 48);
	request.estimated_cost = Max(authz.tool_cost_units * 4, 32);
	request.authorized_tool_tokens = authz.tool_token_cost;
	request.authorized_tool_cost = authz.tool_cost_units;

	selected_contract = qx_runtime_request_phase_contract(&request, false);
	if (selected_contract == NULL)
	{
		QxCatalogFreeAgentInfo(&agent);
		QxCatalogFreeSessionInfo(&session);
		if (tools != NIL)
			list_free_deep(tools);
		if (authz.tool_oids != NIL)
			list_free(authz.tool_oids);
		if (authz.tool_contracts != NIL)
			list_free_deep(authz.tool_contracts);
		if (authz.tool_runtime_classes != NIL)
			list_free_deep(authz.tool_runtime_classes);
		if (authz.tool_sandbox_ceilings != NIL)
			list_free_deep(authz.tool_sandbox_ceilings);
		if (authz.tool_capability_tags != NIL)
			list_free_deep(authz.tool_capability_tags);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("session %u has no authorized tool contract", sessionoid)));
	}

	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	steprel = table_open(QxStepRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);
	queueledgerrel = table_open(QxSchedulerQueueRelationId, RowExclusiveLock);
	leaseledgerrel = table_open(QxSchedulerLeaseRelationId, RowExclusiveLock);
	heartbeatledgerrel = table_open(QxSchedulerHeartbeatRelationId,
									RowExclusiveLock);

	{
		QxCatalogTaskInsertParams task_params;

		qx_runtime_fill_task_insert_params(&task_params, &request);
		taskoid = QxCatalogInsertTask(taskrel, &task_params);
	}
	CommandCounterIncrement();

	attemptoid = QxCatalogInsertAttempt(attemptrel,
								   request.sessionoid,
								   taskoid,
								   request.ownerid,
								   InvalidOid,
								   1,
								   QX_ATTEMPT_STATE_RUNNING,
								   "initial");
	CommandCounterIncrement();

	agent_name = qx_fetch_agent_name(request.agentoid);
	qx_scheduler_fill_envelope(&scheduler_envelope,
							   request.ownerid,
							   request.sessionoid,
							   request.agentoid,
							   request.identityoid,
							   request.namespace_policy_oid,
							   taskoid,
							   attemptoid,
							   request.task_name,
							   agent_name,
							   request.identity_name,
							   request.namespace_policy_name,
							   request.priority,
							   NULL,
							   NULL,
							   false,
							   true,
							   false,
							   request.budget_tokens,
							   request.budget_cost,
							   request.estimated_tokens,
							   request.estimated_cost,
							   0,
							   selected_contract);
	pfree(agent_name);

	queue_snapshot = QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
	queueoid = QxCatalogInsertSchedulerQueue(queueledgerrel, queue_snapshot);
	CommandCounterIncrement();

	payload = psprintf("goal=%s;priority=%s",
					   request.goal,
					   qx_effective_priority_name(request.priority));
	payload = qx_runtime_append_recovery_payload(payload);
	payload = qx_scheduler_append_runtime_payload(payload,
												  &scheduler_envelope,
												  false);
	(void) QxCatalogInsertEvent(eventrel, request.sessionoid, taskoid, InvalidOid,
						   request.ownerid, NULL, "TASK_QUEUED", payload);
	(void) QxCatalogInsertTrace(tracerel, request.sessionoid, taskoid, InvalidOid,
						   request.ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.queue", payload);
	pfree(payload);

	QxCatalogUpdateTaskRuntime(taskrel, taskoid, QX_TASK_STATE_RUNNING,
						   attemptoid, true, InvalidOid, false);

	stepoid = QxCatalogInsertStep(steprel, request.sessionoid, taskoid, 1,
							 "stage8.scheduler_admit",
							 "Scheduler regression hook admitted an initial attempt without writing a durable checkpoint");
	payload = psprintf("attempt_opened;state=%c;identity=%s;namespace_policy=%s;tools=%d;budget_cost=%d;budget_tokens=%d",
					   QX_TASK_STATE_RUNNING,
					   request.identity_name != NULL ? request.identity_name : "<unknown>",
					   request.namespace_policy_name != NULL ? request.namespace_policy_name : "<unknown>",
					   list_length(request.authorized_tools),
					   request.budget_cost,
					   request.budget_tokens);
	payload = qx_runtime_append_recovery_payload(payload);
	payload = qx_scheduler_append_runtime_payload(payload,
												  &scheduler_envelope,
												  true);
	lease_snapshot = QxSchedulerLeaseSnapshotFromEnvelope(&scheduler_envelope,
														 InvalidOid,
														 "embedded-runtime");
	leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel, queueoid,
										 lease_snapshot);
	heartbeat_snapshot = QxSchedulerHeartbeatSnapshotFromLease(lease_snapshot);
	(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel, leaseoid,
										 heartbeat_snapshot);
	CommandCounterIncrement();
	(void) QxCatalogInsertEvent(eventrel, request.sessionoid, taskoid, stepoid,
						   request.ownerid, NULL, "TASK_DISPATCHED", payload);
	(void) QxCatalogInsertTrace(tracerel, request.sessionoid, taskoid, stepoid,
						   request.ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.dispatch", payload);
	pfree(payload);

	table_close(heartbeatledgerrel, RowExclusiveLock);
	table_close(leaseledgerrel, RowExclusiveLock);
	table_close(queueledgerrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);
	table_close(eventrel, RowExclusiveLock);
	table_close(steprel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);

	QxCatalogFreeAgentInfo(&agent);
	QxCatalogFreeSessionInfo(&session);
	if (tools != NIL)
		list_free_deep(tools);
	if (authz.tool_oids != NIL)
		list_free(authz.tool_oids);
	if (authz.tool_contracts != NIL)
		list_free_deep(authz.tool_contracts);
	if (authz.tool_runtime_classes != NIL)
		list_free_deep(authz.tool_runtime_classes);
	if (authz.tool_sandbox_ceilings != NIL)
		list_free_deep(authz.tool_sandbox_ceilings);
	if (authz.tool_capability_tags != NIL)
		list_free_deep(authz.tool_capability_tags);

	return taskoid;
}

static Oid
qx_runtime_test_start_exhausted_retry_task(Oid sessionoid)
{
	Oid			taskoid;
	QxCatalogTaskInfo task;
	QxCatalogAttemptInfo attempt;
	QxSchedulerTaskEnvelope scheduler_envelope;
	QxSchedulerQueueSnapshot *retry_queue;
	Relation	taskrel;
	Relation	attemptrel;
	Relation	eventrel;
	Relation	tracerel;
	Relation	queueledgerrel;
	char	   *selected_contract;
	char	   *payload;
	Oid			queueoid;

	taskoid = qx_runtime_test_start_uncheckpointed_task(sessionoid, "low");
	if (!QxCatalogLookupTaskByOid(taskoid, &task))
		elog(ERROR, "cache lookup failed for QhapaqXian task %u", taskoid);
	if (!QxCatalogLookupAttemptByOid(task.lastattemptid, &attempt))
	{
		QxCatalogFreeTaskInfo(&task);
		elog(ERROR, "cache lookup failed for QhapaqXian attempt %u",
			 task.lastattemptid);
	}

	selected_contract = qx_runtime_recovery_selected_contract(&task, false);
	qx_runtime_fill_recovery_scheduler_envelope(&scheduler_envelope,
												&task,
												attempt.oid,
												qx_runtime_scheduler_max_retries(),
												NULL,
												NULL,
												false,
												true,
												true,
												selected_contract);
	retry_queue = QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
	retry_queue->queue_kind = QX_SCHEDULER_QUEUE_RETRY;
	retry_queue->retry_count = qx_runtime_scheduler_max_retries();
	retry_queue->runnable_count = 1;
	retry_queue->leased_count = 0;
	retry_queue->blocked_count = 0;
	retry_queue->eligible_at = retry_queue->enqueued_at;
	retry_queue->updated_at = retry_queue->enqueued_at;

	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);
	queueledgerrel = table_open(QxSchedulerQueueRelationId, RowExclusiveLock);

	QxCatalogUpdateAttemptState(attemptrel, attempt.oid, QX_ATTEMPT_STATE_FAILED);
	QxCatalogUpdateTaskRuntime(taskrel, taskoid, QX_TASK_STATE_QUEUED,
						   attempt.oid, true, InvalidOid, true);
	queueoid = QxCatalogInsertSchedulerQueue(queueledgerrel, retry_queue);
	CommandCounterIncrement();

	payload = psprintf("test=exhausted_retry;retry_count=%d;max_retries=%d;queueoid=%u",
					   retry_queue->retry_count,
					   qx_runtime_scheduler_max_retries(),
					   queueoid);
	payload = qx_runtime_append_recovery_payload(payload);
	payload = qx_scheduler_append_runtime_payload(payload,
												  &scheduler_envelope,
												  false);
	(void) QxCatalogInsertEvent(eventrel, task.sessionoid, taskoid, InvalidOid,
						   task.ownerid, NULL,
						   "TASK_RETRY_EXHAUSTED_FIXTURE", payload);
	(void) QxCatalogInsertTrace(tracerel, task.sessionoid, taskoid, InvalidOid,
						   task.ownerid, NULL, QX_TRACE_STATE_CLOSED,
						   "runtime.retry_exhausted_fixture", payload);
	pfree(payload);

	table_close(queueledgerrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);
	table_close(eventrel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);

	qx_runtime_free_scheduler_queue_snapshot(retry_queue);
	pfree(retry_queue);
	if (selected_contract != NULL)
		pfree(selected_contract);
	QxCatalogFreeAttemptInfo(&attempt);
	QxCatalogFreeTaskInfo(&task);

	return taskoid;
}

Datum
pg_qx_test_start_uncheckpointed_task(PG_FUNCTION_ARGS)
{
	Oid			sessionoid = PG_GETARG_OID(0);

	PG_RETURN_OID(qx_runtime_test_start_uncheckpointed_task(sessionoid,
															"high"));
}

Datum
pg_qx_test_start_uncheckpointed_task_priority(PG_FUNCTION_ARGS)
{
	Oid			sessionoid = PG_GETARG_OID(0);
	text	   *priority_text = PG_GETARG_TEXT_PP(1);
	char	   *priority = text_to_cstring(priority_text);
	Oid			taskoid;

	taskoid = qx_runtime_test_start_uncheckpointed_task(sessionoid, priority);
	pfree(priority);

	PG_RETURN_OID(taskoid);
}

Datum
pg_qx_test_start_exhausted_retry_task(PG_FUNCTION_ARGS)
{
	Oid			sessionoid = PG_GETARG_OID(0);

	PG_RETURN_OID(qx_runtime_test_start_exhausted_retry_task(sessionoid));
}

Datum
pg_qx_scheduler_worker_slot_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(qx_runtime_scheduler_slot_count());
}

Datum
pg_qx_scheduler_owner_slot(PG_FUNCTION_ARGS)
{
	Oid			taskoid = PG_GETARG_OID(0);
	int32		slot_count = qx_runtime_scheduler_slot_count();

	PG_RETURN_INT32(qx_runtime_scheduler_owner_slot(taskoid, slot_count) + 1);
}

void
QxRuntimeRegisterSchedulerBackgroundWorker(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 1;
	snprintf(worker.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(worker.bgw_function_name, BGW_MAXLEN,
			 "QxRuntimeSchedulerLauncherMain");
	snprintf(worker.bgw_name, BGW_MAXLEN, "qhapaqxian scheduler launcher");
	snprintf(worker.bgw_type, BGW_MAXLEN, "qhapaqxian scheduler launcher");
	worker.bgw_main_arg = Int32GetDatum(0);
	worker.bgw_notify_pid = 0;

	RegisterBackgroundWorker(&worker);
}

void
QxRuntimeSchedulerLauncherMain(Datum main_arg)
{
	List	   *workers = NIL;
	uint32		wait_event = 0;

	(void) main_arg;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	for (;;)
	{
		List	   *targets;
		List	   *alive_workers = NIL;
		ListCell   *lc;
		MemoryContext oldcontext;
		int32		current_slot_count;

		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		pgstat_report_activity(STATE_RUNNING,
							   "qhapaqxian scheduler launcher");
		targets = qx_runtime_scheduler_db_targets();
		current_slot_count = qx_runtime_scheduler_slot_count();

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		foreach(lc, workers)
		{
			QxRuntimeSchedulerDbWorker *worker = lfirst(lc);
			ListCell   *target_lc;
			BgwHandleStatus status;
			pid_t		pid = 0;
			bool		present = false;

			status = GetBackgroundWorkerPid(worker->handle, &pid);
			if (status == BGWH_POSTMASTER_DIED)
			{
				MemoryContextSwitchTo(oldcontext);
				proc_exit(1);
			}
			if (status == BGWH_STOPPED)
			{
				if (worker->dbname != NULL)
					pfree(worker->dbname);
				pfree(worker->handle);
				pfree(worker);
				continue;
			}

			foreach(target_lc, targets)
			{
				QxRuntimeSchedulerDbTarget *target = lfirst(target_lc);
				int32		slot_index;

				if (target->dboid != worker->dboid)
					continue;

				for (slot_index = 0;
					 slot_index < current_slot_count;
					 slot_index++)
				{
					if (worker->slot_index == slot_index &&
						worker->slot_count == current_slot_count)
					{
						present = true;
						break;
					}
				}
				if (present)
					break;
			}

			if (!present)
			{
				TerminateBackgroundWorker(worker->handle);
				if (worker->dbname != NULL)
					pfree(worker->dbname);
				pfree(worker->handle);
				pfree(worker);
				continue;
			}

			alive_workers = lappend(alive_workers, worker);
		}
		list_free(workers);
		workers = alive_workers;

		foreach(lc, targets)
		{
			QxRuntimeSchedulerDbTarget *target = lfirst(lc);
			int32		slot_index;

			for (slot_index = 0;
				 slot_index < current_slot_count;
				 slot_index++)
			{
				QxRuntimeSchedulerDbWorker *worker;
				BackgroundWorkerHandle *handle = NULL;

				if (qx_runtime_find_scheduler_db_worker(workers,
														target->dboid,
														slot_index) != NULL)
					continue;

				if (!qx_runtime_launch_scheduler_db_worker(target->dboid,
														   target->dbname,
														   slot_index,
														   current_slot_count,
														   &handle))
				{
					elog(WARNING,
						 "qhapaqxian scheduler launcher could not start worker for database \"%s\" (%u) slot %d/%d",
						 target->dbname,
						 target->dboid,
						 slot_index + 1,
						 current_slot_count);
					continue;
				}

				worker = MemoryContextAllocZero(TopMemoryContext, sizeof(*worker));
				worker->dboid = target->dboid;
				worker->dbname = MemoryContextStrdup(TopMemoryContext,
													 target->dbname);
				worker->slot_index = slot_index;
				worker->slot_count = current_slot_count;
				worker->handle = handle;
				workers = lappend(workers, worker);
			}
		}
		MemoryContextSwitchTo(oldcontext);

		qx_runtime_free_scheduler_db_targets(targets);
		CommitTransactionCommand();
		pgstat_report_stat(true);
		pgstat_report_activity(STATE_IDLE, NULL);

		qx_runtime_scheduler_heartbeat_wait(&wait_event,
											"QxSchedulerLauncher",
											QX_SCHEDULER_LAUNCHER_NAPTIME_MS);
	}
}

void
QxRuntimeSchedulerDatabaseWorkerMain(Datum main_arg)
{
	QxRuntimeSchedulerCycleStats stats;
	QxRuntimeSchedulerCycleStats retry_stats;
	uint32		wait_event = 0;
	Oid			dboid = InvalidOid;
	QxRuntimeSchedulerWorkerKey key;
	int32		slot_index = 0;
	int32		slot_count = 1;

	MemSet(&key, 0, sizeof(key));
	if (DatumGetUInt32(main_arg) != 0)
		dboid = DatumGetObjectId(main_arg);
	memcpy(&key, MyBgworkerEntry->bgw_extra, sizeof(key));
	if (!OidIsValid(dboid))
		dboid = key.dboid;
	if (key.slot_count > 0)
	{
		slot_index = key.slot_index;
		slot_count = key.slot_count;
	}
	if (!OidIsValid(dboid))
		elog(FATAL, "qhapaqxian scheduler worker missing database OID");

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);

	for (;;)
	{
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		pgstat_report_activity(STATE_RUNNING,
							   "qhapaqxian scheduler supervision");
		qx_runtime_run_scheduler_cycle(&stats, slot_index, slot_count);
		CommitTransactionCommand();
		pgstat_report_stat(true);
		qx_runtime_log_scheduler_cycle(MyBgworkerEntry->bgw_name, &stats);

		PG_TRY();
		{
			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			pgstat_report_activity(STATE_RUNNING,
								   "qhapaqxian scheduler retry intake");
			MemSet(&retry_stats, 0, sizeof(retry_stats));
			qx_runtime_run_scheduler_retry_cycle(&retry_stats,
												slot_index,
												slot_count);
			CommitTransactionCommand();
			pgstat_report_stat(true);
			qx_runtime_log_scheduler_cycle(MyBgworkerEntry->bgw_name,
										  &retry_stats);
		}
		PG_CATCH();
		{
			ErrorData  *edata;
			MemoryContext oldcontext;
			char	   *errmsg;

			oldcontext = MemoryContextSwitchTo(TopMemoryContext);
			edata = CopyErrorData();
			errmsg = pstrdup(edata->message != NULL ? edata->message : "<unknown>");
			MemoryContextSwitchTo(oldcontext);
			FlushErrorState();
			AbortCurrentTransaction();
			elog(WARNING,
				 "qhapaqxian scheduler retry intake failed for database %u: %s",
				 dboid,
				 errmsg);
			pfree(errmsg);
			FreeErrorData(edata);
		}
		PG_END_TRY();

		pgstat_report_activity(STATE_IDLE, NULL);

		qx_runtime_scheduler_heartbeat_wait(&wait_event,
											"QxSchedulerWorker",
											QX_SCHEDULER_WORKER_NAPTIME_MS);
	}
}

Datum
pg_qx_test_run_scheduler_worker_tick(PG_FUNCTION_ARGS)
{
	List	   *tasks;
	ListCell   *lc;
	Relation	leaseledgerrel;
	Relation	heartbeatledgerrel;
	int32		tasks_scanned = 0;
	int32		running_tasks = 0;
	int32		leases_seen = 0;
	int32		leases_renewed = 0;
	int32		heartbeats_written = 0;
	char	   *payload;

	tasks = QxCatalogBuildTaskInfoList(MyDatabaseId, GetUserId());
	leaseledgerrel = table_open(QxSchedulerLeaseRelationId, RowExclusiveLock);
	heartbeatledgerrel = table_open(QxSchedulerHeartbeatRelationId,
									RowExclusiveLock);

	foreach(lc, tasks)
	{
		QxCatalogTaskInfo *task = lfirst(lc);
		QxCatalogAttemptInfo attempt;
		QxSchedulerLeaseSnapshot lease_snapshot;
		QxSchedulerLeaseSnapshot *renewed_lease_snapshot;
		QxSchedulerHeartbeatSnapshot *heartbeat_snapshot;
		Oid			queueoid = InvalidOid;
		Oid			leaseoid;
		int32		lease_ttl_ms;

		tasks_scanned++;
		if (task->state != QX_TASK_STATE_RUNNING ||
			!OidIsValid(task->lastattemptid))
			continue;

		running_tasks++;
		if (!QxCatalogLookupAttemptByOid(task->lastattemptid, &attempt))
			elog(ERROR, "cache lookup failed for QhapaqXian attempt %u",
				 task->lastattemptid);

		if (attempt.state != QX_ATTEMPT_STATE_RUNNING)
		{
			QxCatalogFreeAttemptInfo(&attempt);
			continue;
		}

		MemSet(&lease_snapshot, 0, sizeof(lease_snapshot));
		if (!QxCatalogLookupLatestSchedulerLease(MyDatabaseId, InvalidOid,
									 task->oid,
									 attempt.oid,
									 &lease_snapshot,
									 &queueoid))
		{
			QxCatalogFreeAttemptInfo(&attempt);
			continue;
		}

		if (lease_snapshot.state != QX_SCHEDULER_LEASE_HELD)
		{
			qx_runtime_free_scheduler_lease_snapshot(&lease_snapshot);
			QxCatalogFreeAttemptInfo(&attempt);
			continue;
		}

		leases_seen++;
		lease_ttl_ms = qx_runtime_lease_ttl_ms(&lease_snapshot);
		renewed_lease_snapshot =
			QxSchedulerRenewLeaseSnapshot(&lease_snapshot, lease_ttl_ms);
		leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel,
											 queueoid,
											 renewed_lease_snapshot);
		heartbeat_snapshot =
			QxSchedulerHeartbeatSnapshotFromLease(renewed_lease_snapshot);
		qx_runtime_set_heartbeat_receipt(heartbeat_snapshot,
										 "scheduler-renew");
		(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel,
											 leaseoid,
											 heartbeat_snapshot);
		CommandCounterIncrement();

		leases_renewed++;
		heartbeats_written++;
		qx_runtime_free_scheduler_lease_snapshot(&lease_snapshot);
		QxCatalogFreeAttemptInfo(&attempt);
	}

	table_close(heartbeatledgerrel, RowExclusiveLock);
	table_close(leaseledgerrel, RowExclusiveLock);
	QxCatalogFreeTaskInfoList(tasks);

	payload = psprintf("test=scheduler_worker_tick;tasks_scanned=%d;running_tasks=%d;leases_seen=%d;leases_renewed=%d;heartbeats_written=%d",
					   tasks_scanned,
					   running_tasks,
					   leases_seen,
					   leases_renewed,
					   heartbeats_written);

	PG_RETURN_TEXT_P(cstring_to_text(payload));
}

Datum
pg_qx_recovery_scan(PG_FUNCTION_ARGS)
{
	QxRecoveryStartupRequest request;
	QxRecoveryReport *report;
	TupleDesc	tupdesc;
	Datum		values[12];
	bool		nulls[12];
	HeapTuple	tuple;

	memset(&request, 0, sizeof(request));
	request.databaseoid = MyDatabaseId;
	request.ownerid = GetUserId();
	request.include_attempts = true;
	request.include_checkpoints = true;
	request.fence_stale_attempts = true;
	request.requeue_checkpointed_tasks = true;
	request.rebuild_from_semantic_log = false;

	report = QxRecoveryRunStartupScan(&request, NULL);
	if (report == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("recovery scan did not produce a report")));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));

	values[0] = BoolGetDatum(report->startup_scan);
	values[1] = BoolGetDatum(report->failover_rebuild);
	values[2] = Int32GetDatum(report->tasks_scanned);
	values[3] = Int32GetDatum(report->attempts_scanned);
	values[4] = Int32GetDatum(report->checkpoints_scanned);
	values[5] = Int32GetDatum(report->tasks_requeued);
	values[6] = Int32GetDatum(report->attempts_fenced);
	values[7] = Int32GetDatum(report->checkpoints_replayed);
	values[8] = Int32GetDatum(report->orphan_attempts);
	values[9] = Int32GetDatum(report->semantic_replay_candidates);
	values[10] = Int32GetDatum(report->tasks_requeue_suppressed);
	values[11] = Int32GetDatum(report->attempts_fence_suppressed);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	QxRecoveryFreeReport(report);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

Datum
pg_qx_test_run_startup_recovery(PG_FUNCTION_ARGS)
{
	char	   *payload;

	/*
	 * Regression hook: force a fresh startup recovery pass so tests can observe
	 * ledger writes and collector stats instead of reusing the per-backend cache.
	 */
	qx_runtime_recovery_snapshot.valid = false;
	qx_runtime_ensure_startup_recovery_scan(GetUserId());
	payload = qx_runtime_append_recovery_payload(pstrdup("test=startup_recovery"));

	PG_RETURN_TEXT_P(cstring_to_text(payload));
}

Datum
pg_qx_test_run_failover_rebuild(PG_FUNCTION_ARGS)
{
	QxRecoveryFailoverRequest request;
	QxRecoveryHooks hooks;
	QxRecoveryReport *report;
	QxRuntimeRecoveryHooksContext hook_context;
	char	   *payload;

	memset(&request, 0, sizeof(request));
	request.databaseoid = MyDatabaseId;
	request.ownerid = GetUserId();
	request.rebuild_task_graph = true;
	request.rebuild_attempt_graph = true;
	request.rebuild_checkpoint_graph = true;
	request.replay_semantic_log = true;
	request.fence_orphaned_attempts = true;
	request.requeue_checkpointed_tasks = true;

	memset(&hooks, 0, sizeof(hooks));
	memset(&hook_context, 0, sizeof(hook_context));
	hook_context.ownerid = request.ownerid;
	hooks.begin = qx_runtime_recovery_begin;
	hooks.requeue_task = qx_runtime_recovery_requeue_task;
	hooks.fence_attempt = qx_runtime_recovery_fence_attempt;
	hooks.finish = qx_runtime_recovery_finish;
	hooks.userdata = &hook_context;

	report = QxRecoveryRunFailoverRebuild(&request, &hooks);
	payload = qx_runtime_failover_recovery_payload(
		pstrdup("test=failover_rebuild"), report);
	QxRecoveryFreeReport(report);

	PG_RETURN_TEXT_P(cstring_to_text(payload));
}

Oid
QxRuntimeSubmitTask(const QxRuntimeTaskRequest *request)
{
	Relation	taskrel;
	Relation	attemptrel;
	Relation	steprel;
	Relation	eventrel;
	Relation	tracerel;
	Relation	checkpointrel;
	Relation	queueledgerrel;
	Relation	leaseledgerrel;
	Relation	heartbeatledgerrel;
	Oid			taskoid;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			checkpointoid;
	Oid			queueoid;
	Oid			leaseoid;
	const char *selected_contract;
	char	   *payload;
	char	   *checkpoint_data;
	char	   *agent_name;
	QxSchedulerTaskEnvelope scheduler_envelope;
	QxSchedulerQueueSnapshot *queue_snapshot;
	QxSchedulerLeaseSnapshot *lease_snapshot;
	QxSchedulerHeartbeatSnapshot *heartbeat_snapshot;
	QxSchedulerLeaseSnapshot *released_lease_snapshot;
	QxSchedulerHeartbeatSnapshot *final_heartbeat_snapshot;
	QxExternalToolResult tool_result;
	QxSemanticExecutionMetadata semantic_meta;

	qx_runtime_ensure_startup_recovery_scan(request->ownerid);

	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	steprel = table_open(QxStepRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);
	checkpointrel = table_open(QxCheckpointRelationId, RowExclusiveLock);
	queueledgerrel = table_open(QxSchedulerQueueRelationId, RowExclusiveLock);
	leaseledgerrel = table_open(QxSchedulerLeaseRelationId, RowExclusiveLock);
	heartbeatledgerrel = table_open(QxSchedulerHeartbeatRelationId, RowExclusiveLock);

	{
		QxCatalogTaskInsertParams task_params;

		qx_runtime_fill_task_insert_params(&task_params, request);
		taskoid = QxCatalogInsertTask(taskrel, &task_params);
	}
	CommandCounterIncrement();

	attemptoid = QxCatalogInsertAttempt(attemptrel,
								   request->sessionoid,
								   taskoid,
								   request->ownerid,
								   InvalidOid,
								   1,
								   QX_ATTEMPT_STATE_RUNNING,
								   "initial");
	CommandCounterIncrement();

	selected_contract = qx_runtime_request_phase_contract(request, false);
	agent_name = qx_fetch_agent_name(request->agentoid);
	qx_scheduler_fill_envelope(&scheduler_envelope,
							   request->ownerid,
							   request->sessionoid,
							   request->agentoid,
							   request->identityoid,
							   request->namespace_policy_oid,
							   taskoid,
							   attemptoid,
							   request->task_name,
							   agent_name,
							   request->identity_name,
							   request->namespace_policy_name,
							   request->priority,
							   "stage8.after_capture",
							   NULL,
							   false,
							   true,
							   false,
							   request->budget_tokens,
							   request->budget_cost,
							   request->estimated_tokens,
							   request->estimated_cost,
							   0,
							   selected_contract);
	pfree(agent_name);
	queue_snapshot = QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
	queueoid = QxCatalogInsertSchedulerQueue(queueledgerrel, queue_snapshot);
	CommandCounterIncrement();

	payload = psprintf("goal=%s;priority=%s",
					   request->goal,
					   qx_effective_priority_name(request->priority));
	payload = qx_runtime_append_recovery_payload(payload);
	payload = qx_scheduler_append_runtime_payload(payload,
												  &scheduler_envelope,
												  false);
	(void) QxCatalogInsertEvent(eventrel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, NULL, "TASK_QUEUED", payload);
	(void) QxCatalogInsertTrace(tracerel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.queue", payload);
	pfree(payload);

	QxCatalogUpdateTaskRuntime(taskrel, taskoid, QX_TASK_STATE_RUNNING,
						   attemptoid, true, InvalidOid, false);

	stepoid = QxCatalogInsertStep(steprel, request->sessionoid, taskoid, 1,
							 "stage8.scheduler_admit",
							 "Stage 26 scheduler contract admitted attempt 1 into the embedded runtime");
	QxCatalogChargeTaskBudget(taskrel, taskoid, 16, 17, "scheduler_admit");
	payload = psprintf("attempt_opened;state=%c;identity=%s;namespace_policy=%s;tools=%d;budget_cost=%d;budget_tokens=%d",
					   QX_TASK_STATE_RUNNING,
					   request->identity_name != NULL ? request->identity_name : "<unknown>",
					   request->namespace_policy_name != NULL ? request->namespace_policy_name : "<unknown>",
					   list_length(request->authorized_tools),
					   request->budget_cost,
					   request->budget_tokens);
	payload = qx_runtime_append_recovery_payload(payload);
	payload = qx_scheduler_append_runtime_payload(payload,
												  &scheduler_envelope,
												  true);
	lease_snapshot = QxSchedulerLeaseSnapshotFromEnvelope(&scheduler_envelope,
														 InvalidOid,
														 "embedded-runtime");
	leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel, queueoid,
										 lease_snapshot);
	heartbeat_snapshot = QxSchedulerHeartbeatSnapshotFromLease(lease_snapshot);
	(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel, leaseoid,
										 heartbeat_snapshot);
	CommandCounterIncrement();
	(void) QxCatalogInsertEvent(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, NULL, "TASK_DISPATCHED", payload);
	(void) QxCatalogInsertTrace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.dispatch", payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, request->sessionoid, taskoid, 2,
							 "stage15.authorize_tools",
							 "Attempt 1 validated the runtime tool allowlist against namespace policy");
	payload = psprintf("namespace_policy=%s;authorized_tools=%d;tool_tokens=%d;tool_cost=%d",
					   request->namespace_policy_name != NULL ? request->namespace_policy_name : "<unknown>",
					   list_length(request->authorized_tools),
					   request->authorized_tool_tokens,
					   request->authorized_tool_cost);
	(void) QxCatalogInsertEvent(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, NULL, "TASK_TOOLS_AUTHORIZED",
						   payload);
	(void) QxCatalogInsertTrace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.authorize_tools",
						   payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, request->sessionoid, taskoid, 3,
							 "stage16.external_submit",
							 "Attempt 1 executed the selected tool through its principal program");
	qx_execute_tool_contract(selected_contract, "submit", taskoid,
							 request->goal, request->input != NULL,
							 &tool_result);
	qx_build_semantic_execution_metadata(&semantic_meta, &tool_result);
	QxCatalogChargeTaskBudget(taskrel, taskoid,
						  tool_result.token_charge,
						  tool_result.cost_charge,
						  "external_submit");
	payload = qx_format_external_execution_payload("submit", &tool_result);
	(void) QxCatalogInsertEvent(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, &semantic_meta,
						   "TASK_TOOL_EXECUTED", payload);
	(void) QxCatalogInsertTrace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, &semantic_meta, QX_TRACE_STATE_CLOSED,
						   "runtime.external_submit", payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, request->sessionoid, taskoid, 4,
							 "stage8.capture_input",
							 "Attempt 1 captured task goal and raw input");
	QxCatalogChargeTaskBudget(taskrel, taskoid,
						  request->input != NULL ? 24 : 8,
						  13,
						  "capture_input");
	payload = psprintf("input_present=%s;task_name=%s",
					   request->input != NULL ? "true" : "false",
					   request->task_name != NULL ? request->task_name : "<anonymous>");
	(void) QxCatalogInsertEvent(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, NULL, "TASK_INPUT_CAPTURED",
						   payload);
	(void) QxCatalogInsertTrace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.capture_input",
						   payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, request->sessionoid, taskoid, 5,
							 "stage8.checkpoint_barrier",
							 "Attempt 1 reached a resumable checkpoint barrier");
	QxCatalogChargeTaskBudget(taskrel, taskoid, 4, 14, "checkpoint_barrier");
	payload = psprintf("checkpoint=stage8.after_capture;task_state=%c",
					   QX_TASK_STATE_CHECKPOINTED);
	(void) QxCatalogInsertEvent(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, &semantic_meta,
						   "TASK_CHECKPOINTED", payload);
	(void) QxCatalogInsertTrace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, &semantic_meta, QX_TRACE_STATE_CLOSED,
						   "runtime.checkpoint", payload);

	checkpoint_data = psprintf("task=%u;attempt=%u;session=%u;next_step=%d",
							   taskoid, attemptoid, request->sessionoid, 6);
	checkpointoid = QxCatalogInsertCheckpoint(checkpointrel,
										 request->sessionoid,
										 taskoid,
										 attemptoid,
										 stepoid,
										 request->ownerid,
										 &semantic_meta,
										 QX_TASK_STATE_CHECKPOINTED,
										 6,
										 "stage8.after_capture",
										 checkpoint_data);
	pfree(checkpoint_data);

	QxCatalogUpdateAttemptState(attemptrel, attemptoid, QX_ATTEMPT_STATE_CHECKPOINTED);
	QxCatalogUpdateTaskRuntime(taskrel, taskoid, QX_TASK_STATE_CHECKPOINTED,
						   attemptoid, true, checkpointoid, true);
	released_lease_snapshot = QxSchedulerReleaseLeaseSnapshot(lease_snapshot,
															 false);
	leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel, queueoid,
										 released_lease_snapshot);
	final_heartbeat_snapshot =
		QxSchedulerFinalizeHeartbeatSnapshot(released_lease_snapshot,
											 "scheduler-release");
	(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel, leaseoid,
										 final_heartbeat_snapshot);
	CommandCounterIncrement();
	QxStatReportSchedulerEvent(MyDatabaseId, "release");

	(void) QxCatalogInsertEvent(eventrel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, NULL,
						   "TASK_READY_FOR_RESUME", payload);
	(void) QxCatalogInsertTrace(tracerel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.pause",
						   "Task paused at durable checkpoint and awaits RESUME TASK");
	pfree(payload);
	qx_free_external_result(&tool_result);

	table_close(heartbeatledgerrel, RowExclusiveLock);
	table_close(leaseledgerrel, RowExclusiveLock);
	table_close(queueledgerrel, RowExclusiveLock);
	table_close(checkpointrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);
	table_close(eventrel, RowExclusiveLock);
	table_close(steprel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);

	return taskoid;
}

Oid
QxRuntimeResumeTask(Oid taskoid, const char *checkpoint_label, Oid ownerid)
{
	Relation	taskrel;
	Relation	attemptrel;
	Relation	steprel;
	Relation	eventrel;
	Relation	tracerel;
	Relation	checkpointrel;
	Relation	queueledgerrel;
	Relation	leaseledgerrel;
	Relation	heartbeatledgerrel;
	QxCatalogTaskInfo task;
	QxCatalogCheckpointInfo checkpoint;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			finalcheckpointoid;
	Oid			queueoid;
	Oid			leaseoid;
	int16		nextattemptseqno;
	int16		resume_stepseqno;
	List	   *authorized_contracts;
	char	   *selected_contract;
	char	   *stored_label;
	char	   *payload;
	char	   *checkpoint_data;
	char	   *goal_text;
	bool		input_present;
	QxSchedulerTaskEnvelope scheduler_envelope;
	QxSchedulerQueueSnapshot *queue_snapshot;
	QxSchedulerLeaseSnapshot *lease_snapshot;
	QxSchedulerHeartbeatSnapshot *heartbeat_snapshot;
	QxSchedulerLeaseSnapshot *released_lease_snapshot;
	QxSchedulerHeartbeatSnapshot *final_heartbeat_snapshot;
	QxExternalToolResult tool_result;
	QxSemanticExecutionMetadata semantic_meta;

	if (!QxCatalogLookupTaskByOid(taskoid, &task))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("task %u does not exist", taskoid)));

	if (!has_privs_of_role(ownerid, task.ownerid))
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to resume task %u", taskoid),
				 errdetail("Only the task owner or a member of that role may resume the task.")));
	}

	if (task.state != QX_TASK_STATE_CHECKPOINTED)
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u is not resumable", taskoid),
				 errdetail("RESUME TASK only accepts tasks in checkpointed state.")));
	}

	if (!OidIsValid(task.lastcheckpointid))
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no checkpoint to resume from", taskoid)));
	}

	if (!QxCatalogLookupCheckpointByOid(task.lastcheckpointid, &checkpoint))
	{
		QxCatalogFreeTaskInfo(&task);
		elog(ERROR, "cache lookup failed for QhapaqXian checkpoint %u",
			 task.lastcheckpointid);
	}

	stored_label = checkpoint.label;

	if (checkpoint_label != NULL &&
		(stored_label == NULL || strcmp(checkpoint_label, stored_label) != 0))
	{
		QxCatalogFreeCheckpointInfo(&checkpoint);
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("checkpoint label \"%s\" does not match the current resumable checkpoint for task %u",
						checkpoint_label, taskoid)));
	}

	nextattemptseqno = 1;
	if (OidIsValid(task.lastattemptid))
		nextattemptseqno = qx_fetch_attempt_seqno(task.lastattemptid) + 1;
	resume_stepseqno = checkpoint.next_step_seqno;
	if (resume_stepseqno <= 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("checkpoint \"%s\" for task %u is not resumable",
						stored_label != NULL ? stored_label : "<unnamed>", taskoid),
				 errdetail("The checkpoint does not advertise a valid next step sequence.")));
	}

	qx_runtime_ensure_startup_recovery_scan(ownerid);

	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	steprel = table_open(QxStepRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);
	checkpointrel = table_open(QxCheckpointRelationId, RowExclusiveLock);
	queueledgerrel = table_open(QxSchedulerQueueRelationId, RowExclusiveLock);
	leaseledgerrel = table_open(QxSchedulerLeaseRelationId, RowExclusiveLock);
	heartbeatledgerrel = table_open(QxSchedulerHeartbeatRelationId, RowExclusiveLock);

	attemptoid = QxCatalogInsertAttempt(attemptrel,
								   task.sessionoid,
								   taskoid,
								   ownerid,
								   checkpoint.oid,
								   nextattemptseqno,
								   QX_ATTEMPT_STATE_RUNNING,
								   "resume");
	CommandCounterIncrement();

	QxCatalogUpdateTaskRuntime(taskrel, taskoid, QX_TASK_STATE_RUNNING,
						   attemptoid, true, InvalidOid, false);

	authorized_contracts = qx_parse_task_authorized_contracts(task.authorized_tools);
	selected_contract = qx_runtime_task_phase_contract(&task, true);
	goal_text = task.goal != NULL ? pstrdup(task.goal) : NULL;
	input_present = task.input != NULL;
	qx_scheduler_fill_envelope(&scheduler_envelope,
							   ownerid,
							   task.sessionoid,
							   task.agentoid,
							   task.identityoid,
							   task.namespacepolicyoid,
							   taskoid,
							   attemptoid,
							   task.name,
							   task.agent_name,
							   task.identity_name,
							   task.policy_name,
							   task.priority,
							   "stage8.final",
							   stored_label,
							   false,
							   true,
							   true,
							   task.budget_tokens,
							   task.budget_cost,
							   task.estimated_tokens,
							   task.estimated_cost,
							   nextattemptseqno - 1,
							   selected_contract);
	memset(&tool_result, 0, sizeof(tool_result));
	queue_snapshot = QxSchedulerQueueSnapshotFromEnvelope(&scheduler_envelope);
	queueoid = QxCatalogInsertSchedulerQueue(queueledgerrel, queue_snapshot);
	CommandCounterIncrement();

	payload = psprintf("checkpoint=%s;resume_requested;next_step=%d",
					   stored_label != NULL ? stored_label : "<unnamed>",
					   checkpoint.next_step_seqno);
	payload = qx_runtime_append_recovery_payload(payload);
	payload = qx_scheduler_append_runtime_payload(payload,
												  &scheduler_envelope,
												  false);
	(void) QxCatalogInsertEvent(eventrel, task.sessionoid, taskoid,
						   InvalidOid, ownerid, NULL, "TASK_RESUMED", payload);
	(void) QxCatalogInsertTrace(tracerel, task.sessionoid, taskoid,
						   InvalidOid, ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.resume", payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, task.sessionoid, taskoid,
							 resume_stepseqno,
							 "stage8.resume_dispatch",
							 "Attempt 2 resumed execution from the last durable checkpoint");
	QxCatalogChargeTaskBudget(taskrel, taskoid, 8, 14, "resume_dispatch");
	payload = psprintf("resume_from=%s;state=%c",
					   stored_label != NULL ? stored_label : "<unnamed>",
					   QX_TASK_STATE_RUNNING);
	payload = qx_runtime_append_recovery_payload(payload);
	payload = qx_scheduler_append_runtime_payload(payload,
												  &scheduler_envelope,
												  true);
	lease_snapshot = QxSchedulerLeaseSnapshotFromEnvelope(&scheduler_envelope,
														 InvalidOid,
														 "embedded-runtime");
	leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel, queueoid,
										 lease_snapshot);
	heartbeat_snapshot = QxSchedulerHeartbeatSnapshotFromLease(lease_snapshot);
	(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel, leaseoid,
										 heartbeat_snapshot);
	CommandCounterIncrement();
	(void) QxCatalogInsertEvent(eventrel, task.sessionoid, taskoid,
						   stepoid, ownerid, NULL, "TASK_RESUME_DISPATCHED",
						   payload);
	(void) QxCatalogInsertTrace(tracerel, task.sessionoid, taskoid,
						   stepoid, ownerid, NULL, QX_TRACE_STATE_CLOSED, "runtime.resume_dispatch",
						   payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, task.sessionoid, taskoid,
							 resume_stepseqno + 1,
							 "stage16.external_resume",
							 "Attempt 2 executed the selected tool through its principal program");
	qx_execute_tool_contract(selected_contract, "resume", taskoid,
							 goal_text, input_present, &tool_result);
	qx_build_semantic_execution_metadata(&semantic_meta, &tool_result);
	QxCatalogChargeTaskBudget(taskrel, taskoid,
						  tool_result.token_charge,
						  tool_result.cost_charge,
						  "external_resume");
	payload = qx_format_external_execution_payload("resume", &tool_result);
	(void) QxCatalogInsertEvent(eventrel, task.sessionoid, taskoid,
						   stepoid, ownerid, &semantic_meta,
						   "TASK_TOOL_EXECUTED", payload);
	(void) QxCatalogInsertTrace(tracerel, task.sessionoid, taskoid,
						   stepoid, ownerid, &semantic_meta, QX_TRACE_STATE_CLOSED,
						   "runtime.external_resume", payload);
	pfree(payload);

	stepoid = QxCatalogInsertStep(steprel, task.sessionoid, taskoid,
							 resume_stepseqno + 2,
							 "stage8.complete",
							 "Attempt 2 completed the task after resuming");
	QxCatalogChargeTaskBudget(taskrel, taskoid, 4, 11, "final_checkpoint");
	payload = psprintf("checkpoint=stage8.final;task_state=%c",
					   QX_TASK_STATE_COMPLETED);
	(void) QxCatalogInsertEvent(eventrel, task.sessionoid, taskoid,
						   stepoid, ownerid, &semantic_meta,
						   "TASK_CHECKPOINTED", payload);
	(void) QxCatalogInsertTrace(tracerel, task.sessionoid, taskoid,
						   stepoid, ownerid, &semantic_meta, QX_TRACE_STATE_CLOSED,
						   "runtime.final_checkpoint", payload);

	checkpoint_data = psprintf("task=%u;attempt=%u;session=%u;next_step=%d",
							   taskoid, attemptoid, task.sessionoid, 0);
	finalcheckpointoid = QxCatalogInsertCheckpoint(checkpointrel,
											  task.sessionoid,
											  taskoid,
											  attemptoid,
											  stepoid,
											  ownerid,
											  &semantic_meta,
											  QX_TASK_STATE_COMPLETED,
											  0,
											  "stage8.final",
											  checkpoint_data);
	pfree(checkpoint_data);

	QxCatalogUpdateAttemptState(attemptrel, attemptoid, QX_ATTEMPT_STATE_COMPLETED);
	QxCatalogUpdateTaskRuntime(taskrel, taskoid, QX_TASK_STATE_COMPLETED,
						   attemptoid, true, finalcheckpointoid, true);
	released_lease_snapshot = QxSchedulerReleaseLeaseSnapshot(lease_snapshot,
															 false);
	leaseoid = QxCatalogInsertSchedulerLease(leaseledgerrel, queueoid,
										 released_lease_snapshot);
	final_heartbeat_snapshot =
		QxSchedulerFinalizeHeartbeatSnapshot(released_lease_snapshot,
											 "scheduler-release");
	(void) QxCatalogInsertSchedulerHeartbeat(heartbeatledgerrel, leaseoid,
										 final_heartbeat_snapshot);
	CommandCounterIncrement();
	QxStatReportSchedulerEvent(MyDatabaseId, "release");

	(void) QxCatalogInsertEvent(eventrel, task.sessionoid, taskoid,
						   InvalidOid, ownerid, &semantic_meta,
						   "TASK_COMPLETED", payload);
	(void) QxCatalogInsertTrace(tracerel, task.sessionoid, taskoid,
						   InvalidOid, ownerid, &semantic_meta, QX_TRACE_STATE_CLOSED,
						   "runtime.complete",
						   "Task completed after resumable attempt handoff");
	pfree(payload);
	if (goal_text != NULL)
		pfree(goal_text);
	if (selected_contract != NULL)
		pfree(selected_contract);
	if (authorized_contracts != NIL)
		list_free_deep(authorized_contracts);
	qx_free_external_result(&tool_result);
	QxCatalogFreeCheckpointInfo(&checkpoint);
	QxCatalogFreeTaskInfo(&task);
	table_close(heartbeatledgerrel, RowExclusiveLock);
	table_close(leaseledgerrel, RowExclusiveLock);
	table_close(queueledgerrel, RowExclusiveLock);
	table_close(checkpointrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);
	table_close(eventrel, RowExclusiveLock);
	table_close(steprel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);

	return taskoid;
}

Datum
pg_qx_policy_compile_container(PG_FUNCTION_ARGS)
{
	char	   *capability_tags;
	QxRuntimePolicy policy;
	char	   *profile;

	capability_tags = text_to_cstring(PG_GETARG_TEXT_PP(0));
	qx_runtime_policy_init(&policy);
	qx_runtime_policy_from_authz(NULL, capability_tags, &policy);
	qx_runtime_policy_compile_container(&policy);
	profile = policy.oci_profile != NULL ? pstrdup(policy.oci_profile) : pstrdup("");
	qx_runtime_policy_free(&policy);
	pfree(capability_tags);
	PG_RETURN_TEXT_P(cstring_to_text(profile));
}

Datum
pg_qx_policy_validate_image(PG_FUNCTION_ARGS)
{
	char	   *image_ref;

	image_ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	qx_runtime_policy_validate_image_ref(image_ref);
	pfree(image_ref);
	PG_RETURN_VOID();
}

Datum
pg_qx_policy_validate_microvm_assets(PG_FUNCTION_ARGS)
{
	char	   *kernel;
	char	   *initrd;

	kernel = text_to_cstring(PG_GETARG_TEXT_PP(0));
	initrd = text_to_cstring(PG_GETARG_TEXT_PP(1));
	qx_runtime_policy_validate_microvm_assets(kernel, initrd, NULL);
	pfree(kernel);
	pfree(initrd);
	PG_RETURN_VOID();
}
