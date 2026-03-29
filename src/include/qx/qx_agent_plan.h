/*-------------------------------------------------------------------------
 *
 * qx_agent_plan.h
 *	  metadata-driven agent plan structures for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/qx/qx_agent_plan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_AGENT_PLAN_H
#define QX_AGENT_PLAN_H

#include "nodes/nodes.h"
#include "nodes/pg_list.h"

typedef enum QxAgentPlanKind
{
	QX_AGENT_PLAN_RUN_TASK,
	QX_AGENT_PLAN_RESUME_TASK
} QxAgentPlanKind;

typedef struct QxAgentPlanStep
{
	int16		seqno;
	char	   *name;
	char	   *detail;
	int32		estimated_cost;
	bool		checkpoint_boundary;
	bool		retryable;
} QxAgentPlanStep;

typedef struct QxAgentPlan
{
	QxAgentPlanKind kind;
	Oid			ownerid;
	Oid			agentoid;
	Oid			sessionoid;
	Oid			taskoid;
	Oid			checkpointoid;
	Node	   *input;
	char	   *command_name;
	char	   *task_name;
	char	   *goal;
	char	   *priority;
	char	   *checkpoint_label;
	char	   *target_state;
	char	   *result_mode;
	bool		input_present;
	bool		checkpointable;
	bool		resumable;
	bool		returning_requested;
	int32		estimated_total_cost;
	int32		estimated_tool_calls;
	List	   *steps;			/* list of QxAgentPlanStep */
} QxAgentPlan;

#endif							/* QX_AGENT_PLAN_H */
