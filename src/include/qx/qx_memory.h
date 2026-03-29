/*-------------------------------------------------------------------------
 *
 * qx_memory.h
 *	  memory subsystem interfaces for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/qx/qx_memory.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_MEMORY_H
#define QX_MEMORY_H

#include "access/tupdesc.h"
#include "nodes/parsenodes.h"
#include "tcop/dest.h"

typedef struct QxRememberRequest
{
	Oid			sessionoid;
	Oid			agentoid;
	Oid			ownerid;
	char		scope;
	char	   *memory_key;
	Node	   *memory_value;
	List	   *tags;			/* list of String */
} QxRememberRequest;

extern Oid QxRememberSessionMemory(const QxRememberRequest *request);
extern TupleDesc QxFetchMemoryResultDesc(void);
extern void QxFetchMemoryRecords(Oid agentoid, Oid ownerid, List *scopes,
								 const char *match_text, int limit_count,
								 DestReceiver *dest);
extern const char *QxMemoryScopeLabel(char scope);

#endif							/* QX_MEMORY_H */
