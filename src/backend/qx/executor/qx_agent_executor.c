/*-------------------------------------------------------------------------
 *
 * qx_agent_executor.c
 *	  Stage 8 agent executor boundary for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/executor/qx_agent_executor.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "qx/qx_agent_executor.h"
#include "qx/qx_runtime.h"

Oid
QxExecuteAgentPlan(const QxAgentPlan *plan)
{
	switch (plan->kind)
	{
		case QX_AGENT_PLAN_RUN_TASK:
			{
				QxRuntimeTaskRequest request;

				memset(&request, 0, sizeof(request));
				request.sessionoid = plan->sessionoid;
				request.agentoid = plan->agentoid;
				request.ownerid = plan->ownerid;
				request.task_name = plan->task_name;
				request.goal = plan->goal;
				request.input = plan->input;
				request.priority = plan->priority;

				return QxRuntimeSubmitTask(&request);
			}

		case QX_AGENT_PLAN_RESUME_TASK:
			return QxRuntimeResumeTask(plan->taskoid,
									   plan->checkpoint_label,
									   plan->ownerid);
	}

	elog(ERROR, "unrecognized QhapaqXian plan kind: %d", (int) plan->kind);
	return InvalidOid;
}
