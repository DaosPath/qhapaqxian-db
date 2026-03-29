/*-------------------------------------------------------------------------
 *
 * agentcmds.h
 *	  prototypes for QhapaqXian DB agent commands
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/commands/agentcmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AGENTCMDS_H
#define AGENTCMDS_H

#include "nodes/parsenodes.h"

extern void CreateAgentCommand(CreateAgentStmt *stmt);

#endif							/* AGENTCMDS_H */
