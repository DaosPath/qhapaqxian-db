/*-------------------------------------------------------------------------
 *
 * qx_observe.c
 *	  Stage 28 observability scaffolding for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/observe/qx_observe.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "qx/qx_observe.h"
#include "utils/builtins.h"

static char *qx_observe_contract_value(const char *contract,
									   const char *marker);
static void qx_observe_append_pair(StringInfo buf, const char *key,
								   const char *value);
static void qx_observe_append_bool(StringInfo buf, const char *key,
								   bool value);
static void qx_observe_append_int(StringInfo buf, const char *key,
								  int64 value);
static void qx_observe_assign_text(char **target, const char *value);
static void qx_observe_trace_summary_add_text(char **target,
											  const char *value);

static char *
qx_observe_contract_value(const char *contract, const char *marker)
{
	const char *scan = contract;
	size_t		markerlen;

	if (contract == NULL || marker == NULL || marker[0] == '\0')
		return NULL;

	markerlen = strlen(marker);
	while ((scan = strstr(scan, marker)) != NULL)
	{
		if (scan == contract || scan[-1] == ';' || scan[-1] == '\n' ||
			scan[-1] == ' ')
		{
			const char *value = scan + markerlen;
			const char *end = strpbrk(value, ";\n");

			if (end != NULL)
				return pnstrdup(value, end - value);

			return pstrdup(value);
		}

		scan += markerlen;
	}

	return NULL;
}

static void
qx_observe_append_pair(StringInfo buf, const char *key, const char *value)
{
	if (buf == NULL || key == NULL || value == NULL)
		return;

	if (buf->len > 0)
		appendStringInfoChar(buf, ';');
	appendStringInfo(buf, "%s=%s", key, value);
}

static void
qx_observe_append_bool(StringInfo buf, const char *key, bool value)
{
	qx_observe_append_pair(buf, key, value ? "true" : "false");
}

static void
qx_observe_append_int(StringInfo buf, const char *key, int64 value)
{
	if (buf == NULL || key == NULL)
		return;

	if (buf->len > 0)
		appendStringInfoChar(buf, ';');
	appendStringInfo(buf, "%s=%lld", key, (long long) value);
}

static void
qx_observe_assign_text(char **target, const char *value)
{
	if (target == NULL || *target != NULL || value == NULL || value[0] == '\0')
		return;

	*target = pstrdup(value);
}

static void
qx_observe_trace_summary_add_text(char **target, const char *value)
{
	if (target == NULL || value == NULL || value[0] == '\0')
		return;

	if (*target == NULL)
		*target = pstrdup(value);
}

char *
QxObserveTraceDetailValue(const char *trace_detail, const char *key)
{
	char	   *marker;
	char	   *value;

	if (trace_detail == NULL || key == NULL || key[0] == '\0')
		return NULL;

	marker = psprintf("%s=", key);
	value = qx_observe_contract_value(trace_detail, marker);
	pfree(marker);

	return value;
}

void
QxObserveInitProviderStats(QxObserveProviderStats *stats)
{
	if (stats == NULL)
		return;

	memset(stats, 0, sizeof(*stats));
}

void
QxObserveInitPrincipalStats(QxObservePrincipalStats *stats)
{
	if (stats == NULL)
		return;

	memset(stats, 0, sizeof(*stats));
}

void
QxObserveInitRuntimeClassStats(QxObserveRuntimeClassStats *stats)
{
	if (stats == NULL)
		return;

	memset(stats, 0, sizeof(*stats));
}

void
QxObserveInitTraceSummary(QxObserveTraceSummary *summary)
{
	if (summary == NULL)
		return;

	memset(summary, 0, sizeof(*summary));
}

void
QxObserveRecordProviderExecution(QxObserveProviderStats *stats, bool resume,
								 int32 token_charge, int32 cost_charge,
								 bool receipt_verified)
{
	if (stats == NULL)
		return;

	if (resume)
		stats->resume_count++;
	else
		stats->submit_count++;
	if (receipt_verified)
		stats->verified_receipt_count++;
	else
		stats->rejected_receipt_count++;
	stats->token_used += token_charge;
	stats->cost_used += cost_charge;
}

void
QxObserveRecordPrincipalExecution(QxObservePrincipalStats *stats, bool resume,
								  bool checkpointed, int32 token_charge,
								  int32 cost_charge)
{
	if (stats == NULL)
		return;

	stats->task_count++;
	if (resume)
		stats->resume_count++;
	else
		stats->submit_count++;
	if (checkpointed)
		stats->checkpoint_count++;
	stats->token_used += token_charge;
	stats->cost_used += cost_charge;
}

void
QxObserveRecordRuntimeClassExecution(QxObserveRuntimeClassStats *stats,
									 bool resume, bool checkpointed,
									 int32 token_charge, int32 cost_charge)
{
	if (stats == NULL)
		return;

	stats->task_count++;
	if (resume)
		stats->resume_count++;
	else
		stats->submit_count++;
	if (checkpointed)
		stats->checkpoint_count++;
	stats->token_used += token_charge;
	stats->cost_used += cost_charge;
}

char *
QxObserveSummarizeTraceDetail(const char *trace_name, const char *trace_detail)
{
	StringInfoData buf;
	const char   *keys[] =
	{
		"record_kind",
		"state",
		"phase",
		"tool",
		"scheduler_queue",
		"scheduler_runtime",
		"scheduler_provider",
		"scheduler_provider_kind",
		"scheduler_worker",
		"scheduler_retry",
		"scheduler_checkpoint",
		"scheduler_resume",
		"scheduler_heartbeat_ms",
		"scheduler_lease_ttl_ms",
		"principal",
		"principal_runtime",
		"provider",
		"provider_kind",
		"provider_endpoint",
		"effective_sandbox",
		"profile",
		"env",
		"cwd",
		"timeout_ms",
		"process_limit",
		"receipt_schema",
		"receipt_alg",
		"receipt_nonce",
		"receipt_sig",
		"attestation",
		"launch_mode",
		"restricted_identity",
		"checkpoint",
		"semantic_lsn",
		"wall_ms",
		"tokens",
		"cost"
	};
	int			i;

	initStringInfo(&buf);
	qx_observe_append_pair(&buf, "trace_name",
						   trace_name != NULL ? trace_name : "<unknown>");

	for (i = 0; i < lengthof(keys); i++)
	{
		char	   *value = QxObserveTraceDetailValue(trace_detail, keys[i]);

		if (value != NULL)
		{
			const char *summary_key = keys[i];

			if (strcmp(summary_key, "effective_sandbox") == 0)
				summary_key = "sandbox";
			qx_observe_append_pair(&buf, summary_key, value);
			pfree(value);
		}
	}

	return buf.data;
}

void
QxObserveTraceSummaryAdd(QxObserveTraceSummary *summary, const char *trace_name,
						 const char *trace_detail)
{
	char	   *value;

	if (summary == NULL)
		return;

	summary->record_count++;
	summary->trace_count++;
	qx_observe_assign_text(&summary->trace_name, trace_name);
	if (trace_name != NULL && strcmp(trace_name, "runtime.checkpoint") == 0)
		summary->checkpoint_count++;

	value = qx_observe_contract_value(trace_detail, "record_kind=");
	qx_observe_trace_summary_add_text(&summary->record_kind, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "state=");
	qx_observe_trace_summary_add_text(&summary->trace_state, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "phase=");
	qx_observe_trace_summary_add_text(&summary->phase, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "tool=");
	qx_observe_trace_summary_add_text(&summary->tool_name, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "principal=");
	qx_observe_trace_summary_add_text(&summary->principal_name, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "principal_runtime=");
	qx_observe_trace_summary_add_text(&summary->principal_runtime, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "provider=");
	qx_observe_trace_summary_add_text(&summary->provider_name, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "provider_kind=");
	qx_observe_trace_summary_add_text(&summary->provider_kind, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "provider_endpoint=");
	qx_observe_trace_summary_add_text(&summary->provider_endpoint, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "effective_sandbox=");
	qx_observe_trace_summary_add_text(&summary->sandbox_name, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "profile=");
	qx_observe_trace_summary_add_text(&summary->profile_name, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "env=");
	qx_observe_trace_summary_add_text(&summary->environment_mode, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "cwd=");
	qx_observe_trace_summary_add_text(&summary->workdir_name, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "receipt_schema=");
	qx_observe_trace_summary_add_text(&summary->receipt_schema, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "receipt_alg=");
	qx_observe_trace_summary_add_text(&summary->receipt_alg, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "receipt_nonce=");
	qx_observe_trace_summary_add_text(&summary->receipt_nonce, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "receipt_sig=");
	qx_observe_trace_summary_add_text(&summary->receipt_signature, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "attestation=");
	qx_observe_trace_summary_add_text(&summary->attestation_mode, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "launch_mode=");
	qx_observe_trace_summary_add_text(&summary->launch_mode, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "restricted_identity=");
	qx_observe_trace_summary_add_text(&summary->restricted_identity, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "checkpoint=");
	qx_observe_trace_summary_add_text(&summary->checkpoint_label, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "semantic_lsn=");
	qx_observe_trace_summary_add_text(&summary->semantic_lsn, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "wall_ms=");
	qx_observe_trace_summary_add_text(&summary->wall_time_ms, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "timeout_ms=");
	qx_observe_trace_summary_add_text(&summary->timeout_ms, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "process_limit=");
	qx_observe_trace_summary_add_text(&summary->process_limit, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "tokens=");
	qx_observe_trace_summary_add_text(&summary->token_charge, value);
	if (value != NULL)
		pfree(value);

	value = qx_observe_contract_value(trace_detail, "cost=");
	qx_observe_trace_summary_add_text(&summary->cost_charge, value);
	if (value != NULL)
		pfree(value);
}

void
QxObserveRenderProviderStats(StringInfo buf, const QxObserveProviderStats *stats)
{
	if (buf == NULL || stats == NULL)
		return;

	qx_observe_append_pair(buf, "provider",
						   stats->provider_name != NULL ? stats->provider_name : "<unknown>");
	qx_observe_append_pair(buf, "kind",
						   stats->provider_kind != NULL ? stats->provider_kind : "<unknown>");
	qx_observe_append_pair(buf, "endpoint",
						   stats->provider_endpoint != NULL ? stats->provider_endpoint : "<unknown>");
	qx_observe_append_bool(buf, "enabled", stats->enabled);
	qx_observe_append_bool(buf, "attestation_required",
						   stats->attestation_required);
	qx_observe_append_int(buf, "principal_count", stats->principal_count);
	qx_observe_append_int(buf, "submit_count", stats->submit_count);
	qx_observe_append_int(buf, "resume_count", stats->resume_count);
	qx_observe_append_int(buf, "verified_receipts",
						  stats->verified_receipt_count);
	qx_observe_append_int(buf, "rejected_receipts",
						  stats->rejected_receipt_count);
	qx_observe_append_int(buf, "token_budget", stats->token_budget);
	qx_observe_append_int(buf, "token_used", stats->token_used);
	qx_observe_append_int(buf, "cost_budget", stats->cost_budget);
	qx_observe_append_int(buf, "cost_used", stats->cost_used);
}

void
QxObserveRenderPrincipalStats(StringInfo buf, const QxObservePrincipalStats *stats)
{
	if (buf == NULL || stats == NULL)
		return;

	qx_observe_append_pair(buf, "principal",
						   stats->principal_name != NULL ? stats->principal_name : "<unknown>");
	qx_observe_append_pair(buf, "provider",
						   stats->provider_name != NULL ? stats->provider_name : "<unknown>");
	qx_observe_append_pair(buf, "provider_kind",
						   stats->provider_kind != NULL ? stats->provider_kind : "<unknown>");
	qx_observe_append_pair(buf, "runtime_class",
						   stats->runtime_class != NULL ? stats->runtime_class : "<unknown>");
	qx_observe_append_pair(buf, "sandbox",
						   stats->sandbox_name != NULL ? stats->sandbox_name : "<unknown>");
	qx_observe_append_pair(buf, "program",
						   stats->program_name != NULL ? stats->program_name : "<unknown>");
	qx_observe_append_bool(buf, "enabled", stats->enabled);
	qx_observe_append_bool(buf, "has_signer", stats->has_signer);
	qx_observe_append_int(buf, "task_count", stats->task_count);
	qx_observe_append_int(buf, "submit_count", stats->submit_count);
	qx_observe_append_int(buf, "resume_count", stats->resume_count);
	qx_observe_append_int(buf, "checkpoint_count", stats->checkpoint_count);
	qx_observe_append_int(buf, "token_budget", stats->token_budget);
	qx_observe_append_int(buf, "token_used", stats->token_used);
	qx_observe_append_int(buf, "cost_budget", stats->cost_budget);
	qx_observe_append_int(buf, "cost_used", stats->cost_used);
}

void
QxObserveRenderRuntimeClassStats(StringInfo buf,
								 const QxObserveRuntimeClassStats *stats)
{
	if (buf == NULL || stats == NULL)
		return;

	qx_observe_append_pair(buf, "runtime_class",
						   stats->runtime_class != NULL ? stats->runtime_class : "<unknown>");
	qx_observe_append_int(buf, "provider_count", stats->provider_count);
	qx_observe_append_int(buf, "principal_count", stats->principal_count);
	qx_observe_append_int(buf, "task_count", stats->task_count);
	qx_observe_append_int(buf, "submit_count", stats->submit_count);
	qx_observe_append_int(buf, "resume_count", stats->resume_count);
	qx_observe_append_int(buf, "checkpoint_count", stats->checkpoint_count);
	qx_observe_append_int(buf, "token_budget", stats->token_budget);
	qx_observe_append_int(buf, "token_used", stats->token_used);
	qx_observe_append_int(buf, "cost_budget", stats->cost_budget);
	qx_observe_append_int(buf, "cost_used", stats->cost_used);
}

void
QxObserveRenderTraceSummary(StringInfo buf, const QxObserveTraceSummary *summary)
{
	if (buf == NULL || summary == NULL)
		return;

	qx_observe_append_pair(buf, "trace_name",
						   summary->trace_name != NULL ? summary->trace_name : "<unknown>");
	qx_observe_append_pair(buf, "record_kind",
						   summary->record_kind != NULL ? summary->record_kind : "<unknown>");
	qx_observe_append_pair(buf, "trace_state",
						   summary->trace_state != NULL ? summary->trace_state : "<unknown>");
	qx_observe_append_pair(buf, "phase",
						   summary->phase != NULL ? summary->phase : "<unknown>");
	qx_observe_append_pair(buf, "tool",
						   summary->tool_name != NULL ? summary->tool_name : "<unknown>");
	qx_observe_append_pair(buf, "principal",
						   summary->principal_name != NULL ? summary->principal_name : "<unknown>");
	qx_observe_append_pair(buf, "principal_runtime",
						   summary->principal_runtime != NULL ? summary->principal_runtime : "<unknown>");
	qx_observe_append_pair(buf, "provider",
						   summary->provider_name != NULL ? summary->provider_name : "<unknown>");
	qx_observe_append_pair(buf, "provider_kind",
						   summary->provider_kind != NULL ? summary->provider_kind : "<unknown>");
	qx_observe_append_pair(buf, "provider_endpoint",
						   summary->provider_endpoint != NULL ? summary->provider_endpoint : "<unknown>");
	qx_observe_append_pair(buf, "sandbox",
						   summary->sandbox_name != NULL ? summary->sandbox_name : "<unknown>");
	qx_observe_append_pair(buf, "profile",
						   summary->profile_name != NULL ? summary->profile_name : "<unknown>");
	qx_observe_append_pair(buf, "env",
						   summary->environment_mode != NULL ? summary->environment_mode : "<unknown>");
	qx_observe_append_pair(buf, "cwd",
						   summary->workdir_name != NULL ? summary->workdir_name : "<unknown>");
	qx_observe_append_pair(buf, "receipt_schema",
						   summary->receipt_schema != NULL ? summary->receipt_schema : "<unknown>");
	qx_observe_append_pair(buf, "receipt_alg",
						   summary->receipt_alg != NULL ? summary->receipt_alg : "<unknown>");
	qx_observe_append_pair(buf, "receipt_nonce",
						   summary->receipt_nonce != NULL ? summary->receipt_nonce : "<unknown>");
	qx_observe_append_pair(buf, "receipt_sig",
						   summary->receipt_signature != NULL ? summary->receipt_signature : "<unknown>");
	qx_observe_append_pair(buf, "attestation",
						   summary->attestation_mode != NULL ? summary->attestation_mode : "<unknown>");
	qx_observe_append_pair(buf, "launch_mode",
						   summary->launch_mode != NULL ? summary->launch_mode : "<unknown>");
	qx_observe_append_pair(buf, "restricted_identity",
						   summary->restricted_identity != NULL ? summary->restricted_identity : "<unknown>");
	qx_observe_append_pair(buf, "checkpoint",
						   summary->checkpoint_label != NULL ? summary->checkpoint_label : "<unknown>");
	qx_observe_append_pair(buf, "semantic_lsn",
						   summary->semantic_lsn != NULL ? summary->semantic_lsn : "<unknown>");
	qx_observe_append_pair(buf, "wall_ms",
						   summary->wall_time_ms != NULL ? summary->wall_time_ms : "<unknown>");
	qx_observe_append_pair(buf, "timeout_ms",
						   summary->timeout_ms != NULL ? summary->timeout_ms : "<unknown>");
	qx_observe_append_pair(buf, "process_limit",
						   summary->process_limit != NULL ? summary->process_limit : "<unknown>");
	qx_observe_append_pair(buf, "tokens",
						   summary->token_charge != NULL ? summary->token_charge : "<unknown>");
	qx_observe_append_pair(buf, "cost",
						   summary->cost_charge != NULL ? summary->cost_charge : "<unknown>");
	qx_observe_append_int(buf, "record_count", summary->record_count);
	qx_observe_append_int(buf, "event_count", summary->event_count);
	qx_observe_append_int(buf, "trace_count", summary->trace_count);
	qx_observe_append_int(buf, "checkpoint_count", summary->checkpoint_count);
}

Datum
pg_qx_trace_detail_value(PG_FUNCTION_ARGS)
{
	char	   *trace_detail = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *value = QxObserveTraceDetailValue(trace_detail, key);

	if (value == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(value));
}
