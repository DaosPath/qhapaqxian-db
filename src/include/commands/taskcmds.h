/*-------------------------------------------------------------------------
 *
 * taskcmds.h
 *	  prototypes for QhapaqXian DB task commands
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/commands/taskcmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TASKCMDS_H
#define TASKCMDS_H

#include "nodes/parsenodes.h"

extern void RunTaskCommand(RunTaskStmt *stmt);
extern void ResumeTaskCommand(ResumeTaskStmt *stmt);

#endif							/* TASKCMDS_H */
