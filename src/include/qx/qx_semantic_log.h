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

extern XLogRecPtr QxEmitSemanticEventRecord(Oid eventoid, Oid sessionoid,
											Oid taskoid, Oid stepoid,
											Oid ownerid, const char *kind,
											const char *payload);
extern XLogRecPtr QxEmitSemanticTraceRecord(Oid traceoid, Oid sessionoid,
											Oid taskoid, Oid stepoid,
											Oid ownerid, char state,
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

#endif							/* QX_SEMANTIC_LOG_H */
