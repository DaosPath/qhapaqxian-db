/*-------------------------------------------------------------------------
 *
 * qx_runtime.h
 *	  runtime entry points for QhapaqXian Engine task execution
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/qx/qx_runtime.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_RUNTIME_H
#define QX_RUNTIME_H

#include "nodes/nodes.h"

typedef struct QxRuntimeTaskRequest
{
	Oid			sessionoid;
	Oid			agentoid;
	Oid			ownerid;
	const char *task_name;
	const char *goal;
	Node	   *input;
	const char *priority;
} QxRuntimeTaskRequest;

extern Oid QxRuntimeSubmitTask(const QxRuntimeTaskRequest *request);
extern Oid QxRuntimeResumeTask(Oid taskoid, const char *checkpoint_label,
							   Oid ownerid);

#endif							/* QX_RUNTIME_H */
