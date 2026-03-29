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
				request.identityoid = plan->identityoid;
				request.namespace_policy_oid = plan->namespace_policy_oid;
				request.ownerid = plan->ownerid;
				request.task_name = plan->task_name;
				request.identity_name = plan->identity_name;
				request.namespace_policy_name = plan->namespace_policy_name;
				request.goal = plan->goal;
				request.input = plan->input;
				request.priority = plan->priority;
				request.authorized_tools = plan->authorized_tools;
				request.budget_tokens = plan->budget_tokens;
				request.budget_cost = plan->budget_cost;
				request.estimated_tokens = plan->estimated_tokens;
				request.estimated_cost = plan->estimated_total_cost;
				request.authorized_tool_tokens = plan->authorized_tool_tokens;
				request.authorized_tool_cost = plan->authorized_tool_cost;

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
