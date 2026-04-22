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
	Oid			identityoid;
	Oid			namespace_policy_oid;
	Oid			ownerid;
	const char *task_name;
	const char *identity_name;
	const char *namespace_policy_name;
	const char *goal;
	Node	   *input;
	const char *priority;
	List	   *authorized_tools;
	List	   *authorized_tool_oids;
	const char *submit_contract;
	const char *resume_contract;
	int32		budget_tokens;
	int32		budget_cost;
	int32		estimated_tokens;
	int32		estimated_cost;
	int32		authorized_tool_tokens;
	int32		authorized_tool_cost;
} QxRuntimeTaskRequest;

extern Oid QxRuntimeSubmitTask(const QxRuntimeTaskRequest *request);
extern Oid QxRuntimeResumeTask(Oid taskoid, const char *checkpoint_label,
							   Oid ownerid);
extern void QxRuntimeRegisterSchedulerBackgroundWorker(void);
extern void QxRuntimeSchedulerLauncherMain(Datum main_arg) pg_attribute_noreturn();
extern void QxRuntimeSchedulerDatabaseWorkerMain(Datum main_arg) pg_attribute_noreturn();

#endif							/* QX_RUNTIME_H */
