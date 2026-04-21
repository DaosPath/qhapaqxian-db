/*-------------------------------------------------------------------------
 *
 * qx_semantic_log.h
 *	  semantic WAL/logical message helpers for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/qx/qx_semantic_log.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_SEMANTIC_LOG_H
#define QX_SEMANTIC_LOG_H

#include "access/xlogdefs.h"

typedef struct QxSemanticExecutionMetadata
{
	const char *provider_name;
	const char *provider_kind;
	const char *provider_endpoint;
	const char *principal_name;
	const char *principal_runtime;
	const char *sandbox_name;
	const char *profile_name;
	const char *environment_mode;
	const char *workdir_name;
	const char *launch_mode;
	const char *receipt_schema;
	const char *receipt_alg;
	const char *receipt_nonce;
	const char *attestation_mode;
	int32		timeout_ms;
	int32		process_limit;
	int32		wall_time_ms;
	int32		token_charge;
	int32		cost_charge;
	bool		path_present;
	bool		restricted_identity;
} QxSemanticExecutionMetadata;

extern XLogRecPtr QxEmitSemanticEventRecord(Oid eventoid, Oid sessionoid,
											Oid taskoid, Oid stepoid,
											Oid ownerid, const char *kind,
											const char *payload);
extern XLogRecPtr QxEmitSemanticEventRecordV2(Oid eventoid, Oid sessionoid,
											  Oid taskoid, Oid stepoid,
											  Oid ownerid,
											  const QxSemanticExecutionMetadata *metadata,
											  const char *kind,
											  const char *payload);
extern XLogRecPtr QxEmitSemanticTraceRecord(Oid traceoid, Oid sessionoid,
											Oid taskoid, Oid stepoid,
											Oid ownerid, char state,
											const char *name,
											const char *detail);
extern XLogRecPtr QxEmitSemanticTraceRecordV2(Oid traceoid, Oid sessionoid,
											  Oid taskoid, Oid stepoid,
											  Oid ownerid,
											  const QxSemanticExecutionMetadata *metadata,
											  char state,
											  const char *name,
											  const char *detail);
extern XLogRecPtr QxEmitSemanticCheckpointRecord(Oid checkpointoid,
												 Oid sessionoid, Oid taskoid,
												 Oid attemptoid, Oid stepoid,
												 Oid ownerid,
												 char checkpoint_state,
												 char task_state,
												 int16 nextstepseqno,
												 const char *label,
												 const char *data);
extern XLogRecPtr QxEmitSemanticCheckpointRecordV2(Oid checkpointoid,
												   Oid sessionoid, Oid taskoid,
												   Oid attemptoid, Oid stepoid,
												   Oid ownerid,
												   const QxSemanticExecutionMetadata *metadata,
												   char checkpoint_state,
												   char task_state,
												   int16 nextstepseqno,
												   const char *label,
												   const char *data);

#endif							/* QX_SEMANTIC_LOG_H */
