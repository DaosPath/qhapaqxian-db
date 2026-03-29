/*-------------------------------------------------------------------------
 *
 * qx_agent_planner.h
 *	  planner interfaces for metadata-driven agent plans
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/qx/qx_agent_planner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_AGENT_PLANNER_H
#define QX_AGENT_PLANNER_H

#include "access/tupdesc.h"
#include "nodes/parsenodes.h"
#include "qx/qx_agent_plan.h"
#include "tcop/dest.h"

extern QxAgentPlan *QxBuildAgentPlan(Node *stmt, Oid ownerid);
extern TupleDesc QxExplainAgentResultDesc(void);
extern void QxExplainAgentCommand(ExplainAgentStmt *stmt, DestReceiver *dest);

#endif							/* QX_AGENT_PLANNER_H */
