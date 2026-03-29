/*-------------------------------------------------------------------------
 *
 * qx_agent_executor.h
 *	  executor interfaces for metadata-driven agent plans
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/qx/qx_agent_executor.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_AGENT_EXECUTOR_H
#define QX_AGENT_EXECUTOR_H

#include "qx/qx_agent_plan.h"

extern Oid QxExecuteAgentPlan(const QxAgentPlan *plan);

#endif							/* QX_AGENT_EXECUTOR_H */
