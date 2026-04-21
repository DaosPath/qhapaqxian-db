/*-------------------------------------------------------------------------
 *
 * qx_observe.h
 *	  observability contracts and trace summaries for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/qx/qx_observe.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_OBSERVE_H
#define QX_OBSERVE_H

#include "lib/stringinfo.h"

typedef struct QxObserveProviderStats
{
	Oid			provider_oid;
	Oid			namespace_oid;
	Oid			owner_oid;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *provider_endpoint;
	char	   *receipt_alg;
	bool		enabled;
	bool		attestation_required;
	int64		principal_count;
	int64		submit_count;
	int64		resume_count;
	int64		verified_receipt_count;
	int64		rejected_receipt_count;
	int64		token_budget;
	int64		token_used;
	int64		cost_budget;
	int64		cost_used;
} QxObserveProviderStats;

typedef struct QxObservePrincipalStats
{
	Oid			principal_oid;
	Oid			namespace_oid;
	Oid			provider_oid;
	Oid			owner_oid;
	char	   *principal_name;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *runtime_class;
	char	   *sandbox_name;
	char	   *program_name;
	char	   *receipt_signer;
	bool		enabled;
	bool		has_signer;
	int64		task_count;
	int64		submit_count;
	int64		resume_count;
	int64		checkpoint_count;
	int64		token_budget;
	int64		token_used;
	int64		cost_budget;
	int64		cost_used;
} QxObservePrincipalStats;

typedef struct QxObserveRuntimeClassStats
{
	char	   *runtime_class;
	int64		provider_count;
	int64		principal_count;
	int64		task_count;
	int64		submit_count;
	int64		resume_count;
	int64		checkpoint_count;
	int64		token_budget;
	int64		token_used;
	int64		cost_budget;
	int64		cost_used;
} QxObserveRuntimeClassStats;

typedef struct QxObserveTraceSummary
{
	char	   *trace_name;
	char	   *record_kind;
	char	   *trace_state;
	char	   *phase;
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
	char	   *receipt_schema;
	char	   *receipt_alg;
	char	   *receipt_nonce;
	char	   *receipt_signature;
	char	   *attestation_mode;
	char	   *launch_mode;
	char	   *restricted_identity;
	char	   *checkpoint_label;
	char	   *semantic_lsn;
	char	   *wall_time_ms;
	char	   *timeout_ms;
	char	   *process_limit;
	char	   *token_charge;
	char	   *cost_charge;
	int64		record_count;
	int64		event_count;
	int64		trace_count;
	int64		checkpoint_count;
} QxObserveTraceSummary;

extern void QxObserveInitProviderStats(QxObserveProviderStats *stats);
extern void QxObserveInitPrincipalStats(QxObservePrincipalStats *stats);
extern void QxObserveInitRuntimeClassStats(QxObserveRuntimeClassStats *stats);
extern void QxObserveInitTraceSummary(QxObserveTraceSummary *summary);

extern void QxObserveRecordProviderExecution(QxObserveProviderStats *stats,
											 bool resume,
											 int32 token_charge,
											 int32 cost_charge,
											 bool receipt_verified);
extern void QxObserveRecordPrincipalExecution(QxObservePrincipalStats *stats,
											  bool resume,
											  bool checkpointed,
											  int32 token_charge,
											  int32 cost_charge);
extern void QxObserveRecordRuntimeClassExecution(QxObserveRuntimeClassStats *stats,
												 bool resume,
												 bool checkpointed,
												 int32 token_charge,
												 int32 cost_charge);

extern char *QxObserveTraceDetailValue(const char *trace_detail,
									   const char *key);
extern char *QxObserveSummarizeTraceDetail(const char *trace_name,
										   const char *trace_detail);
extern void QxObserveTraceSummaryAdd(QxObserveTraceSummary *summary,
									 const char *trace_name,
									 const char *trace_detail);

extern void QxObserveRenderProviderStats(StringInfo buf,
										 const QxObserveProviderStats *stats);
extern void QxObserveRenderPrincipalStats(StringInfo buf,
										  const QxObservePrincipalStats *stats);
extern void QxObserveRenderRuntimeClassStats(StringInfo buf,
											 const QxObserveRuntimeClassStats *stats);
extern void QxObserveRenderTraceSummary(StringInfo buf,
										const QxObserveTraceSummary *summary);

#endif							/* QX_OBSERVE_H */
