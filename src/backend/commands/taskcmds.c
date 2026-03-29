/*-------------------------------------------------------------------------
 *
 * taskcmds.c
 *	  task command entry points for QhapaqXian DB
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/commands/taskcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/taskcmds.h"
#include "miscadmin.h"
#include "qx/qx_agent_executor.h"
#include "qx/qx_agent_planner.h"

void
RunTaskCommand(RunTaskStmt *stmt)
{
	QxAgentPlan *plan;

	if (stmt->returning)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("RUN TASK ... RETURNING TASK is not executable yet"),
				 errdetail("Etapa 8 routes task execution through AgentPlan, but tuple-returning utility execution is still deferred."),
				 errhint("Inspect pg_qx_task and related pg_qx_* catalogs after RUN TASK, or proceed to later stages for tuple-returning execution.")));
	}

	plan = QxBuildAgentPlan((Node *) stmt, GetUserId());
	(void) QxExecuteAgentPlan(plan);
}

void
ResumeTaskCommand(ResumeTaskStmt *stmt)
{
	QxAgentPlan *plan;

	plan = QxBuildAgentPlan((Node *) stmt, GetUserId());
	(void) QxExecuteAgentPlan(plan);
}
